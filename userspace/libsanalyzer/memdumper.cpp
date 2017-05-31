#include <sys/types.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <iostream>
#include <sinsp.h>
#include <sinsp_int.h>
#include "utils.h"
#ifdef HAS_ANALYZER
#include "draios.pb.h"
#include "analyzer_int.h"
#include "analyzer.h"
#endif
#include <memdumper.h>

extern sinsp_evttables g_infotables;

sinsp_memory_dumper::sinsp_memory_dumper(sinsp* inspector, bool capture_dragent_events)
{
	m_inspector = inspector;
	m_file_id = 0;
	m_f = NULL;
	m_cf = NULL;
	m_disabled = false;
	m_switches_to_go = 0;
	m_saturation_inactivity_start_time = 0;
	m_delayed_switch_states_needed = false;
	m_delayed_switch_states_ready = false;

#if defined(HAS_CAPTURE)
	m_capture_dragent_events = capture_dragent_events;
	if(!capture_dragent_events)
	{
		m_sysdig_pid = m_inspector->m_sysdig_pid;
	}
#endif
}

void sinsp_memory_dumper::init(uint64_t bufsize,
	uint64_t max_disk_size,
	uint64_t saturation_inactivity_pause_ns)
{
	lo(sinsp_logger::SEV_INFO, "memdumper: initializing memdumper, bufsize=%" PRIu64 ", max_disk_size=%" PRIu64 ", saturation_inactivity_pause_ns=%" PRIu64,
		bufsize,
		max_disk_size,
		saturation_inactivity_pause_ns);

	m_max_disk_size = max_disk_size;
	m_saturation_inactivity_pause_ns = saturation_inactivity_pause_ns;

	//
	// Let the inspector know that we're dumping
	//
	m_inspector->m_is_dumping = true;

	//
	// Initialize the buffers. In the common case, we use 2 memory
	// buffers. There is a possibility of using 3 when there
	// are simultaneous readers and writers of the memory
	// buffer. Hence the dividing by 3.
	//
	m_bsize = bufsize / 3;

	// 2 states
	for(uint32_t i=0; i < 2; i++)
	{
		string errstr;
		string name = "/dragent-memdumper-" + to_string(m_file_id++);
		std::shared_ptr<sinsp_memory_dumper_state> state = make_shared<sinsp_memory_dumper_state>(m_inspector, m_bsize, name);
		m_states.emplace_back(state);

		if(!state->open(errstr))
		{
			lo(sinsp_logger::SEV_ERROR, "memdump: could not open memdumer state %s: %s. Memory dump disabled", name.c_str(), errstr.c_str());
			m_disabled = true;
		}

	}
	m_active_state = m_states.begin();

	// No reader right now
	m_reader_state = m_states.rend();

/*
	//
	// Initialize the dump file
	//
	char tbuf[32768];
	struct timeval ts;
	gettimeofday(&ts, NULL);
	time_t rawtime = (time_t)ts.tv_sec;
	struct tm* time_info = gmtime(&rawtime);
	snprintf(tbuf, sizeof(tbuf), "%.2d-%.2d_%.2d_%.2d_%.2d_%.6d",
		time_info->tm_mon + 1,
		time_info->tm_mday,
		time_info->tm_hour,
		time_info->tm_min,
		time_info->tm_sec,
		(int)ts.tv_usec);

	string fname = string("sd_dump_") + tbuf + ".scap";

	m_f = fopen(fname.c_str(), "wb");
	if(m_f == NULL)
	{
		lo(sinsp_logger::SEV_ERROR, "memdumper: cannot open file %s", fname.c_str());
	}
*/
	m_f = NULL;

	//
	// Initialize the notification event
	//
	m_notification_scap_evt = (scap_evt*)m_notification_scap_evt_storage;
	m_notification_scap_evt->type = PPME_NOTIFICATION_E;
	m_notification_evt.m_poriginal_evt = NULL;
	m_notification_evt.m_pevt = m_notification_scap_evt;
	m_notification_evt.m_inspector = m_inspector;
}

sinsp_memory_dumper::~sinsp_memory_dumper()
{
	if(m_f != NULL)
	{
		fclose(m_f);
	}

	if(m_cf != NULL)
	{
		fclose(m_cf);
	}
}

void sinsp_memory_dumper::close()
{
	switch_states(0);
	m_inspector->m_is_dumping = false;
}

void sinsp_memory_dumper::to_file_multi(string name, uint64_t ts_ns)
{
	char tbuf[512];

	m_switches_to_go = 2;

	if(m_cf != NULL)
	{
		return;
	}

	if((m_saturation_inactivity_start_time != 0) &&
		(ts_ns - m_saturation_inactivity_start_time) < m_saturation_inactivity_pause_ns)
	{
		return;
	}

	time_t rawtime = (time_t)ts_ns / 1000000000;
	struct tm* time_info = gmtime(&rawtime);
	snprintf(tbuf, sizeof(tbuf), "%.2d-%.2d_%.2d_%.2d_%.2d_%.6d",
		time_info->tm_mon + 1,
		time_info->tm_mday,
		time_info->tm_hour,
		time_info->tm_min,
		time_info->tm_sec,
		(int)(ts_ns % 1000000000));

	string fname = string("/tmp/sd_dump_") + name + "_" + tbuf + ".scap";

	lo(sinsp_logger::SEV_INFO, "memdumper: saving dump %s", fname.c_str());

	m_cf = fopen(fname.c_str(), "wb");
	if(m_cf == NULL)
	{
		lo(sinsp_logger::SEV_ERROR,
			"memdumper: cannot open file %s, dump will not happen", fname.c_str());
		return;
	}

//	sinsp_memory_dumper_state* inactive_state =
//		(m_active_state == &m_states[0])? &m_states[1] : &m_states[0];

//	fclose(m_cf);
//	m_cf = NULL;

	m_cur_dump_size = m_bsize;
}

// Read as much of the shared memory buffer held in state as possible
// using the provided inspector.
bool sinsp_memory_dumper::read_membuf_using_inspector(sinsp &inspector,
						      const std::shared_ptr<sinsp_memory_dumper_state> &state,
						      sinsp_memory_dumper_job* job)
{
	int32_t res;
	sinsp_evt* ev;

	// Flush the dumper state, which also returns the number of
	// bytes written so far. Don't read past this point.
	uint64_t dumper_bytes_written = state->flush();

	// Force a seek to 0 and back to our position to ensure that
	// no cached read data is kept--we may have read data from the
	// file that was stale and updated by the flush we just did.
	uint64_t bytes_read = inspector.get_bytes_read();
	inspector.fseek(0);
	inspector.fseek(bytes_read);

	lo(sinsp_logger::SEV_DEBUG, "memdumper: reading %s from pos %lu to %lu",
	   state->m_shm_name.c_str(),
	   inspector.get_bytes_read(), dumper_bytes_written);

	while(inspector.get_bytes_read() < dumper_bytes_written)
	{
		res = inspector.next(&ev);

		if(res == SCAP_EOF || job->m_state == sinsp_memory_dumper_job::ST_STOPPPED)
		{
			break;
		}
		else if(res == SCAP_TIMEOUT)
		{
			continue;
		}
		else if(res != SCAP_SUCCESS && res != SCAP_TIMEOUT)
		{
			//
			// There was an error. Just stop here and log the error
			//
			job->m_state = sinsp_memory_dumper_job::ST_DONE_ERROR;
			job->m_lasterr = "apply_job_filter error reading events from file " + state->m_shm_name + ": " + inspector.getlasterr();
			ASSERT(false);
			return false;
		}

		if(job->m_start_time != 0 && ev->get_ts() < job->m_start_time)
		{
			continue;
		}

		if(job->m_dumper == NULL)
		{
			job->m_dumper = new sinsp_dumper(&inspector);
			try
			{
				job->m_dumper->open(job->m_filename, false, true);
			}
			catch(exception &e)
			{
				job->m_lasterr = "inspector could not open dump file " + job->m_filename + ". inspector_err=" + inspector.getlasterr() + " e=" + e.what();
				job->m_state = sinsp_memory_dumper_job::ST_DONE_ERROR;
				return false;
			}
		}

		job->m_n_events++;
		job->m_dumper->dump(ev);
	}

	return true;
}

void sinsp_memory_dumper::apply_job_filter(const shared_ptr<sinsp_memory_dumper_state> &state,
					   sinsp_memory_dumper_job* job,
					   Poco::Mutex *membuf_mtx)
{
	if (!state->is_open() || state->m_dumper->written_events() == 0)
	{
		return;
	}

	// If the timerange of this memory buffer doesn't overlap with
	// the timerange of the job, return immediately
	if (job->m_start_time != 0 && state->m_end_ts < job->m_start_time)
	{
		return;
	}

	sinsp inspector;
	inspector.set_hostname_and_port_resolution_mode(false);

	// Open the shared memory segment again so we can read from
	// the beginning.
	int fd = shm_open(state->m_shm_name.c_str(), O_RDONLY, 0);
	if(fd == -1)
	{
		job->m_lasterr = "Could not open shared memory region " + state->m_shm_name + " for reading: " + strerror(errno);
		job->m_state = sinsp_memory_dumper_job::ST_DONE_ERROR;
		return;
	}

	// Flush state to disk so an inspector reading the
	// same shm file will have its initial state.
	state->flush();

	try
	{
		inspector.fdopen(fd);
	}
	catch(exception &e)
	{
		job->m_lasterr = "inspector could not open shared memory region. inspector_err=" + inspector.getlasterr() + " e=" + e.what() + " nevt=" + to_string((*m_active_state)->m_dumper->written_events());
		job->m_state = sinsp_memory_dumper_job::ST_DONE_ERROR;
		::close(fd);
		return;
	}

	if(job->m_filterstr != "")
	{
		string flt = "(" + job->m_filterstr + ") or evt.type=notification";
		inspector.set_filter(flt);
	}

	if (!read_membuf_using_inspector(inspector, state, job))
	{
		inspector.close();
		::close(fd);
		return;
	}

	// Now check the offset and read again. If we're currently
	// reading the active state, lock the membuf mutex now, so no
	// additional events can again until unlocked (by the caller
	// of add_job()).
	{
		Poco::ScopedLock<Poco::FastMutex> lck(m_state_mtx);
		if(membuf_mtx && (*m_active_state)->m_shm_name == state->m_shm_name)
		{
			lo(sinsp_logger::SEV_DEBUG, "memdumper: Approaching end of state=%s, locking membuf mutex", state->m_shm_name.c_str());
			membuf_mtx->lock();
		}
	}

	if (!read_membuf_using_inspector(inspector, state, job))
	{
		// When returning a failure, don't keep the mutex locked.
		if(membuf_mtx)
		{
			membuf_mtx->unlock();
		}
		inspector.close();
		::close(fd);
		return;
	}

	inspector.close();
	::close(fd);
}

sinsp_memory_dumper_job* sinsp_memory_dumper::add_job(uint64_t ts, string filename, string filter,
						      uint64_t delta_time_past_ns, uint64_t delta_time_future_ns,
						      bool track_job, Poco::Mutex *membuf_mtx)
{
	struct timeval tm;
	gettimeofday(&tm, NULL);

	sinsp_memory_dumper_job* job = new sinsp_memory_dumper_job();
	if(track_job)
	{
		m_jobs.push_back(job);
	}

	job->m_start_time =
		delta_time_past_ns != 0? ts - delta_time_past_ns : 0;
	job->m_end_time = ts + delta_time_future_ns;
	job->m_filename = filename;

	if(filter != "")
	{
		job->m_filterstr = filter;

		try
		{
			sinsp_filter_compiler compiler(m_inspector, filter);
			job->m_filter = compiler.compile();
		}
		catch(exception &e)
		{
			job->m_state = sinsp_memory_dumper_job::ST_DONE_ERROR;
			job->m_lasterr = "error compiling capture job filter (" + filter + "). e=" + e.what();
			return job;
		}
	}

	{
		Poco::ScopedLock<Poco::FastMutex> lck(m_state_mtx);
		m_reader_state = m_states.rbegin();
	}

	while(m_reader_state != m_states.rend())
	{
		apply_job_filter(*m_reader_state, job, membuf_mtx);
		{
			Poco::ScopedLock<Poco::FastMutex> lck(m_state_mtx);
			m_reader_state++;
		}
	}

	// It's possible (although unlikely) that while reading
	// through the memory buffers it was necessary to create a
	// temporary third buffer. In this case, remove the oldest
	// buffer.
	{
		Poco::ScopedLock<Poco::FastMutex> lck(m_state_mtx);
		while (m_states.size() > 2)
		{
			lo(sinsp_logger::SEV_DEBUG, "memdumper: Removing temporary additional state while reader was active");
			m_states.pop_back();
		}
	}

	// If process_event was waiting for a delayed state switch,
	// allow it now.
	if(m_delayed_switch_states_needed)
	{
		m_delayed_switch_states_ready = true;
	}

	//
	// If no capture in the future is required, the job can stop here
	//
	if(delta_time_future_ns == 0)
	{
		job->m_state = sinsp_memory_dumper_job::ST_DONE_OK;
	}

	return job;
}

void sinsp_memory_dumper::remove_job(sinsp_memory_dumper_job* job)
{
	m_jobs.erase(std::remove(m_jobs.begin(), m_jobs.end(), job), m_jobs.end());
	delete job;
}

void sinsp_memory_dumper::switch_states(uint64_t ts)
{
	Poco::ScopedLock<Poco::FastMutex> lck(m_state_mtx);

	lo(sinsp_logger::SEV_DEBUG, "memdumper: switching memory buffer states");

	// If a delayed switch was needed, it's no longer needed. Log
	// an error with the number of missed events.

	if(m_delayed_switch_states_needed)
	{
		lo(sinsp_logger::SEV_WARNING, "memdumper: missed %lu events waiting for new job creation to finish", m_delayed_switch_states_missed_events);
		m_delayed_switch_states_needed = false;
		m_delayed_switch_states_ready = false;
		m_delayed_switch_states_missed_events = 0;
	}

	//
	// Save the capture to disk
	//
	if(m_cf)
	{
//		flush_state_to_disk(m_cf, m_active_state, false);
		m_cur_dump_size += m_bsize;
		m_switches_to_go--;

		bool toobig = (m_cur_dump_size >= m_max_disk_size);

		if(m_switches_to_go == 0 ||
			toobig)
		{
			fclose(m_cf);
			m_cf = NULL;

			if(toobig)
			{
				m_saturation_inactivity_start_time = ts;
				lo(sinsp_logger::SEV_INFO, "memdumper: dump closed because too big, m_max_disk_size=%" PRIu64 ", waiting %" PRIu64 " ns",
					m_max_disk_size,
					m_saturation_inactivity_pause_ns);
			}
			else
			{
				lo(sinsp_logger::SEV_INFO, "memdumper: dump closed");
			}
		}
	}

	// If a reader is reading the last state, create a new state
	// and put it at the front. However, never create more than 3
	// states. If there are already 3 states, simply skip event
	// processing until the reader has read all the states and
	// brought the total down to 2.

	// Otherwise, take the last state and put it at the front.
	if(m_reader_state == m_states.rbegin())
	{
		if(m_states.size() < 3)
		{
			lo(sinsp_logger::SEV_DEBUG, "memdumper: creating temporary additional state while reader is active");
			string name = "/dragent-memdumper-" + to_string(m_file_id++);
			m_states.emplace_front(make_shared<sinsp_memory_dumper_state>(m_inspector, m_bsize, name));
		}
		else
		{
			lo(sinsp_logger::SEV_WARNING, "memdumper: stopping event processing while new job creation is active");
			m_delayed_switch_states_needed = true;
			m_delayed_switch_states_ready = false;
			m_delayed_switch_states_missed_events = 0;
		}
	}
	else
	{
		shared_ptr<sinsp_memory_dumper_state> st = m_states.back();
		m_states.pop_back();
		m_states.push_front(st);
	}

	m_active_state = m_states.begin();

	// Reopen the first state
	string errstr;
	if (!(*m_active_state)->open(errstr))
	{
		lo(sinsp_logger::SEV_ERROR, "memdumper: could not reopen swapped state: %s. Memory dump disabled", errstr.c_str());
		m_disabled = true;
		return;
	}
}

void sinsp_memory_dumper::push_notification(uint64_t ts, uint64_t tid, string id, string description)
{
	m_notification_scap_evt->ts = ts;
	m_notification_scap_evt->tid = tid;

	uint16_t *lens = (uint16_t *)(m_notification_scap_evt_storage + sizeof(struct ppm_evt_hdr));
	uint16_t idlen = id.length() + 1;
	uint16_t desclen = description.length() + 1;
	lens[0] = idlen;
	lens[1] = desclen;

	memcpy((m_notification_scap_evt_storage + sizeof(struct ppm_evt_hdr) + 4),
		id.c_str(),
		idlen);

	memcpy((m_notification_scap_evt_storage + sizeof(struct ppm_evt_hdr) + 4 + idlen),
		description.c_str(),
		desclen);

	m_notification_scap_evt->len = sizeof(scap_evt) + sizeof(uint16_t) + 4 + idlen + desclen + 1;

	process_event(&m_notification_evt);
}
