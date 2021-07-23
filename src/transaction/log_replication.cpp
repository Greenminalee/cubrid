/*
 * Copyright 2008 Search Solution Corporation
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#include "log_replication.hpp"

#include "btree_load.h"
#include "log_impl.h"
#include "log_recovery.h"
#include "log_recovery_redo.hpp"
#include "log_recovery_redo_parallel.hpp"
#include "object_representation.h"
#include "page_buffer.h"
#include "recovery.h"
#include "thread_looper.hpp"
#include "thread_manager.hpp"
#include "transaction_global.hpp"
#include "util_func.h"

#include <cassert>
#include <chrono>
#include <functional>

namespace cublog
{
  /*********************************************************************
   * replication delay calculation - declaration
   *********************************************************************/
  static int log_rpl_calculate_replication_delay (THREAD_ENTRY *thread_p, time_t a_start_time_msec);

  /* job implementation that performs log replication delay calculation
   * using log records that register creation time
   */
  class redo_job_replication_delay_impl final : public redo_parallel::redo_job_base
  {
      /* sentinel VPID value needed for the internal mechanics of the parallel log recovery/replication
       * internally, such a VPID is needed to maintain absolute order of the processing
       * of the log records with respect to their order in the global log record
       */
      static constexpr vpid SENTINEL_VPID = { -2, -2 };

    public:
      redo_job_replication_delay_impl (const log_lsa &a_rcv_lsa, time_msec_t a_start_time_msec);

      redo_job_replication_delay_impl (redo_job_replication_delay_impl const &) = delete;
      redo_job_replication_delay_impl (redo_job_replication_delay_impl &&) = delete;

      ~redo_job_replication_delay_impl () override = default;

      redo_job_replication_delay_impl &operator = (redo_job_replication_delay_impl const &) = delete;
      redo_job_replication_delay_impl &operator = (redo_job_replication_delay_impl &&) = delete;

      int execute (THREAD_ENTRY *thread_p, log_reader &log_pgptr_reader,
		   LOG_ZIP &undo_unzip_support, LOG_ZIP &redo_unzip_support) override;

    private:
      const time_msec_t m_start_time_msec;
  };

  /*********************************************************************
   * replication b-tree unique statistics - declaration
   *********************************************************************/

  // replicate_btree_stats does redo record simulation
  static void replicate_btree_stats (cubthread::entry &thread_entry, const VPID &root_vpid,
				     const log_unique_stats &stats, const log_lsa &record_lsa);

  // a job for replication b-tree stats update
  class redo_job_btree_stats : public redo_parallel::redo_job_base
  {
    public:
      redo_job_btree_stats (const VPID &vpid, const log_lsa &record_lsa, const log_unique_stats &stats);

      int execute (THREAD_ENTRY *thread_p, log_reader &log_pgptr_reader, LOG_ZIP &undo_unzip_support,
		   LOG_ZIP &redo_unzip_support) override;

    private:
      log_unique_stats m_stats;
  };

  replicator::replicator (const log_lsa &start_redo_lsa)
    : m_redo_lsa { start_redo_lsa }
    , m_perfmon_redo_sync { PSTAT_REDO_REPL_LOG_REDO_SYNC }
    , m_rcv_redo_perf_stat { false }
  {
    log_zip_realloc_if_needed (m_undo_unzip, LOGAREA_SIZE);
    log_zip_realloc_if_needed (m_redo_unzip, LOGAREA_SIZE);

    // depending on parameter, instantiate the mechanism to execute replication in parallel
    // mandatory to initialize before daemon such that:
    //  - race conditions, when daemon comes online, are avoided
    //  - (even making abstraction of the race conditions) no log records are needlessly
    //    processed synchronously
    const int replication_parallel
      = prm_get_integer_value (PRM_ID_REPLICATION_PARALLEL_COUNT);
    assert (replication_parallel >= 0);
    if (replication_parallel > 0)
      {
	m_minimum_log_lsa.reset (new cublog::minimum_log_lsa_monitor ());
	// no need to reset with start redo lsa

	m_parallel_replication_redo.reset (new cublog::redo_parallel (replication_parallel, m_minimum_log_lsa.get ()));
      }

    // Create the daemon
    cubthread::looper loop (std::chrono::milliseconds (1));   // don't spin when there is no new log, wait a bit
    auto func_exec = std::bind (&replicator::redo_upto_nxio_lsa, std::ref (*this), std::placeholders::_1);

    auto func_retire = std::bind (&replicator::conclude_task_execution, std::ref (*this));
    // initialized with explicit 'exec' and 'retire' functors, the ownership of the daemon task
    // done not reside with the task itself (aka, the task does not get to delete itself anymore);
    // therefore store it in in pointer such that we can be sure it is disposed of sometime towards the end
    m_daemon_task.reset (new cubthread::entry_callable_task (std::move (func_exec), std::move (func_retire)));

    m_daemon_context_manager = std::make_unique<cubthread::system_worker_entry_manager> (TT_REPLICATION);

    m_daemon = cubthread::get_manager ()->create_daemon (loop, m_daemon_task.get (), "cublog::replicator",
	       m_daemon_context_manager.get ());
  }

  replicator::~replicator ()
  {
    cubthread::get_manager ()->destroy_daemon (m_daemon);

    if (m_parallel_replication_redo != nullptr)
      {
	// this is the earliest it is ensured that no records are to be added anymore
	m_parallel_replication_redo->set_adding_finished ();
	m_parallel_replication_redo->wait_for_termination_and_stop_execution ();
      }

    log_zip_free_data (m_undo_unzip);
    log_zip_free_data (m_redo_unzip);
  }

  void
  replicator::redo_upto_nxio_lsa (cubthread::entry &thread_entry)
  {
    thread_entry.tran_index = LOG_SYSTEM_TRAN_INDEX;

    while (true)
      {
	const log_lsa nxio_lsa = log_Gl.append.get_nxio_lsa ();
	if (m_redo_lsa < nxio_lsa)
	  {
	    redo_upto (thread_entry, nxio_lsa);
	  }
	else
	  {
	    assert (m_redo_lsa == nxio_lsa);
	    break;
	  }
      }
  }

  void
  replicator::conclude_task_execution ()
  {
    if (m_parallel_replication_redo != nullptr)
      {
	// without being aware of external context/factors, this is the earliest it is ensured that
	// no records are to be added anymore
	m_parallel_replication_redo->wait_for_idle ();
      }
    else
      {
	// nothing needs to be done in the synchronous execution scenario
	// the default/internal implementation of the retire functor used to delete the task
	// itself; this is now handled by the instantiating entity
      }
  }

  void
  replicator::redo_upto (cubthread::entry &thread_entry, const log_lsa &end_redo_lsa)
  {
    assert (m_redo_lsa < end_redo_lsa);

    // redo all records from current position (m_redo_lsa) until end_redo_lsa

    m_perfmon_redo_sync.start ();
    // make sure the log page is refreshed. otherwise it may be outdated and new records may be missed
    m_reader.set_lsa_and_fetch_page (m_redo_lsa, log_reader::fetch_mode::FORCE);

    while (m_redo_lsa < end_redo_lsa)
      {
	// read and redo a record
	m_reader.set_lsa_and_fetch_page (m_redo_lsa);

	const log_rec_header header = m_reader.reinterpret_copy_and_add_align<log_rec_header> ();

	switch (header.type)
	  {
	  case LOG_REDO_DATA:
	    read_and_redo_record<log_rec_redo> (thread_entry, header.type, m_redo_lsa);
	    break;
	  case LOG_MVCC_REDO_DATA:
	    read_and_redo_record<log_rec_mvcc_redo> (thread_entry, header.type, m_redo_lsa);
	    break;
	  case LOG_UNDOREDO_DATA:
	  case LOG_DIFF_UNDOREDO_DATA:
	    read_and_redo_record<log_rec_undoredo> (thread_entry, header.type, m_redo_lsa);
	    break;
	  case LOG_MVCC_UNDOREDO_DATA:
	  case LOG_MVCC_DIFF_UNDOREDO_DATA:
	    read_and_redo_record<log_rec_mvcc_undoredo> (thread_entry, header.type, m_redo_lsa);
	    break;
	  case LOG_RUN_POSTPONE:
	    read_and_redo_record<log_rec_run_postpone> (thread_entry, header.type, m_redo_lsa);
	    break;
	  case LOG_COMPENSATE:
	    read_and_redo_record<log_rec_compensate> (thread_entry, header.type, m_redo_lsa);
	    break;
	  case LOG_DBEXTERN_REDO_DATA:
	  {
	    const log_rec_dbout_redo dbout_redo = m_reader.reinterpret_copy_and_add_align<log_rec_dbout_redo> ();
	    log_rcv rcv;
	    rcv.length = dbout_redo.length;

	    log_rv_redo_record (&thread_entry, m_reader, RV_fun[dbout_redo.rcvindex].redofun, &rcv, &m_redo_lsa, 0,
				nullptr, m_redo_unzip);
	    break;
	  }
	  case LOG_COMMIT:
	    calculate_replication_delay_or_dispatch_async<log_rec_donetime> (
		    thread_entry, m_redo_lsa);
	    break;
	  case LOG_ABORT:
	    calculate_replication_delay_or_dispatch_async<log_rec_donetime> (
		    thread_entry, m_redo_lsa);
	    break;
	  case LOG_DUMMY_HA_SERVER_STATE:
	    calculate_replication_delay_or_dispatch_async<log_rec_ha_server_state> (
		    thread_entry, m_redo_lsa);
	    break;
	  default:
	    // do nothing
	    break;
	  }

	{
	  std::unique_lock<std::mutex> lock (m_redo_lsa_mutex);
	  m_redo_lsa = header.forw_lsa;
	}
	if (m_parallel_replication_redo != nullptr)
	  {
	    m_minimum_log_lsa->set_for_outer (m_redo_lsa);
	  }

	// to accurately track progress and avoid clients to wait for too long, notify each change
	m_redo_lsa_condvar.notify_all ();

	m_perfmon_redo_sync.track_and_start ();
      }
  }

  template <typename T>
  void
  replicator::read_and_redo_btree_stats (cubthread::entry &thread_entry, LOG_RECTYPE rectype, const log_lsa &rec_lsa,
					 const T &log_rec)
  {
    //
    // Recovery redo does not apply b-tree stats directly into the b-tree root page. But while replicating on the page
    // server, we have to update the statistics directly into the root page, because it may be fetched by a transaction
    // server and stats have to be up-to-date at all times.
    //
    // To redo the change directly into the root page, we need to simulate having a redo job on the page and we need
    // the page VPID. The VPID is obtained from the redo data of the log record. Therefore, the redo data must be read
    // first, then a special job is created with all required information.
    //

    // Get redo data and read it
    LOG_RCV rcv;
    rcv.length = log_rv_get_log_rec_redo_length<T> (log_rec);
    if (log_rv_get_log_rec_redo_data (&thread_entry, m_reader, log_rec, rcv, rectype, m_undo_unzip, m_redo_unzip)
	!= NO_ERROR)
      {
	logpb_fatal_error (&thread_entry, true, ARG_FILE_LINE, "replicator::read_and_redo_btree_stats");
	return;
      }
    BTID btid;
    log_unique_stats stats;
    btree_rv_data_get_btid_and_stats (rcv, btid, stats);
    VPID root_vpid = { btid.root_pageid, btid.vfid.volid };

    // Create a job or apply the change immediately
    if (m_parallel_replication_redo)
      {
	auto job = std::make_unique<redo_job_btree_stats> (root_vpid, rec_lsa, stats);
	m_parallel_replication_redo->add (std::move (job));
      }
    else
      {
	replicate_btree_stats (thread_entry, root_vpid, stats, rec_lsa);
      }
  }

  template <typename T>
  void
  replicator::read_and_redo_record (cubthread::entry &thread_entry, LOG_RECTYPE rectype, const log_lsa &rec_lsa)
  {
    m_reader.advance_when_does_not_fit (sizeof (T));
    const T log_rec = m_reader.reinterpret_copy_and_add_align<T> ();

    // To allow reads on the page server, make sure that all changes are visible.
    // Having log_Gl.hdr.mvcc_next_id higher than all MVCCID's in the database is a requirement.
    MVCCID mvccid = log_rv_get_log_rec_mvccid (log_rec);
    if (mvccid != MVCCID_NULL && !MVCC_ID_PRECEDES (mvccid, log_Gl.hdr.mvcc_next_id))
      {
	log_Gl.hdr.mvcc_next_id = mvccid;
	MVCCID_FORWARD (log_Gl.hdr.mvcc_next_id);
      }

    // Redo b-tree stats differs from what the recovery usually does. Get the recovery index before deciding how to
    // proceed.
    LOG_RCVINDEX rcvindex = log_rv_get_log_rec_data (log_rec).rcvindex;
    if (rcvindex == RVBT_LOG_GLOBAL_UNIQUE_STATS_COMMIT)
      {
	read_and_redo_btree_stats (thread_entry, rectype, rec_lsa, log_rec);
      }
    else
      {
	log_rv_redo_record_sync_or_dispatch_async (&thread_entry, m_reader, log_rec, rec_lsa, nullptr, rectype,
	    m_undo_unzip, m_redo_unzip, m_parallel_replication_redo, true, m_rcv_redo_perf_stat);
      }
  }

  template <typename T>
  void replicator::calculate_replication_delay_or_dispatch_async (cubthread::entry &thread_entry,
      const log_lsa &rec_lsa)
  {
    const T log_rec = m_reader.reinterpret_copy_and_add_align<T> ();
    // at_time, expressed in milliseconds rather than seconds
    const time_msec_t start_time_msec = log_rec.at_time;
    if (m_parallel_replication_redo != nullptr)
      {
	// dispatch a job; the time difference will be calculated when the job is actually
	// picked up for completion by a task; this will give an accurate estimate of the actual
	// delay between log generation on the page server and log recovery on the page server
	std::unique_ptr<cublog::redo_job_replication_delay_impl> replication_delay_job
	{
	  new cublog::redo_job_replication_delay_impl (m_redo_lsa, start_time_msec)
	};
	m_parallel_replication_redo->add (std::move (replication_delay_job));
      }
    else
      {
	// calculate the time difference synchronously
	log_rpl_calculate_replication_delay (&thread_entry, start_time_msec);
      }
  }

  void
  replicator::wait_replication_finish_during_shutdown () const
  {
    std::unique_lock<std::mutex> ulock (m_redo_lsa_mutex);
    m_redo_lsa_condvar.wait (ulock, [this]
    {
      return m_redo_lsa >= log_Gl.append.get_nxio_lsa ();
    });

    // at this moment, ALL data has been dispatched for, either, async replication
    // or has been applied synchronously
    // introduce a fuzzy syncronization point by waiting all fed data to be effectively
    // consumed/applied
    // however, since the daemon is still running, also leave the parallel replication
    // logic (if instantiated) alive; will be destroyed only after the daemon (to maintain
    // symmetry with instantiation)
    if (m_parallel_replication_redo != nullptr)
      {
	m_parallel_replication_redo->wait_for_idle ();
      }
  }

  void replicator::wait_past_target_lsa (const log_lsa &a_target_lsa)
  {
    if (m_parallel_replication_redo == nullptr)
      {
	// sync
	std::unique_lock<std::mutex> ulock { m_redo_lsa_mutex };
	m_redo_lsa_condvar.wait (ulock, [this, a_target_lsa] ()
	{
	  return m_redo_lsa > a_target_lsa;
	});
      }
    else
      {
	// async
	m_minimum_log_lsa->wait_past_target_lsa (a_target_lsa);
      }
  }

  /*********************************************************************
   * redo_job_replication_delay_impl - definition
   *********************************************************************/

  redo_job_replication_delay_impl::redo_job_replication_delay_impl (
	  const log_lsa &a_rcv_lsa, time_msec_t a_start_time_msec)
    : redo_parallel::redo_job_base (SENTINEL_VPID, a_rcv_lsa)
    , m_start_time_msec (a_start_time_msec)
  {
  }

  int
  redo_job_replication_delay_impl::execute (THREAD_ENTRY *thread_p, log_reader &, LOG_ZIP &, LOG_ZIP &)
  {
    const int res = log_rpl_calculate_replication_delay (thread_p, m_start_time_msec);
    return res;
  }

  /* log_rpl_calculate_replication_delay - calculate delay based on a given start time value
   *        and the current time and log to the perfmon infrastructure; all calculations are
   *        done in milliseconds as that is the relevant scale needed
   */
  int
  log_rpl_calculate_replication_delay (THREAD_ENTRY *thread_p, time_msec_t a_start_time_msec)
  {
    // skip calculation if bogus input (sometimes, it is -1);
    // TODO: fix bogus input at the source if at all possible (debugging revealed that
    // it happens for LOG_COMMIT messages only and there is no point at the source where the 'at_time'
    // is not filled in)
    if (a_start_time_msec > 0)
      {
	const int64_t end_time_msec = util_get_time_as_ms_since_epoch ();
	const int64_t time_diff_msec = end_time_msec - a_start_time_msec;
	assert (time_diff_msec >= 0);

	perfmon_set_stat (thread_p, PSTAT_REDO_REPL_DELAY, static_cast<int> (time_diff_msec), false);

	if (prm_get_bool_value (PRM_ID_ER_LOG_CALC_REPL_DELAY))
	  {
	    _er_log_debug (ARG_FILE_LINE, "[CALC_REPL_DELAY]: %9lld msec", time_diff_msec);
	  }

	return NO_ERROR;
      }
    else
      {
	er_log_debug (ARG_FILE_LINE, "log_rpl_calculate_replication_delay: "
		      "encountered negative start time value: %lld milliseconds",
		      a_start_time_msec);
	return ER_FAILED;
      }
  }

  /*********************************************************************
   * replication b-tree unique statistics - declaration
   *********************************************************************/

  void
  replicate_btree_stats (cubthread::entry &thread_entry, const VPID &root_vpid, const log_unique_stats &stats,
			 const log_lsa &record_lsa)
  {
    PAGE_PTR root_page = log_rv_redo_fix_page (&thread_entry, &root_vpid, RVBT_LOG_GLOBAL_UNIQUE_STATS_COMMIT);
    if (root_page == nullptr)
      {
	logpb_fatal_error (&thread_entry, true, ARG_FILE_LINE, "cublog::replicate_btree_stats");
	return;
      }

    btree_root_update_stats (&thread_entry, root_page, stats);
    pgbuf_set_lsa (&thread_entry, root_page, &record_lsa);
    pgbuf_set_dirty_and_free (&thread_entry, root_page);
  }

  redo_job_btree_stats::redo_job_btree_stats (const VPID &vpid, const log_lsa &record_lsa, const log_unique_stats &stats)
    : redo_parallel::redo_job_base (vpid, record_lsa)
    , m_stats (stats)
  {
  }

  int
  redo_job_btree_stats::execute (THREAD_ENTRY *thread_p, log_reader &, LOG_ZIP &, LOG_ZIP &)
  {
    replicate_btree_stats (*thread_p, get_vpid (), m_stats, get_log_lsa ());
    return NO_ERROR;
  }

} // namespace cublog
