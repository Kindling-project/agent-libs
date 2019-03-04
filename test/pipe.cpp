#define VISIBILITY_PRIVATE

#include <sinsp.h>
#include <sinsp_int.h>
#include <analyzer.h>
#include <analyzer_settings.h>
#include <connectinfo.h>
#include "sys_call_test.h"
#include <gtest.h>
#include <algorithm>
#include "event_capture.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <event.h>
#include <Poco/Process.h>
#include <Poco/PipeStream.h>
#include <Poco/StringTokenizer.h>
#include <Poco/NumberFormatter.h>
#include <list>
#include <cassert>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>

using namespace std;
using Poco::StringTokenizer;
using Poco::NumberFormatter;

#define PAYLOAD         "0123456789QWERTYUIOPASDFGHJKLZXCVBNM"
#define BUFFER_LENGTH    (sizeof(PAYLOAD) - 1)

#define FIFO_NAME "/tmp/myfifo"
#define MAX_BUF 1024
class named_pipe_writer
{
public:
	named_pipe_writer()
	{
		m_tid = 0;
	}

	void run()
	{
		int fd;
		m_tid = syscall(SYS_gettid);

		/* create the FIFO (named pipe) */
		mkfifo(FIFO_NAME, 0666);
		m_ready.set();
		/* write "Hi" to the FIFO */
		fd = open(FIFO_NAME, O_WRONLY);
		ASSERT_TRUE(write(fd, "Hi", sizeof("Hi")) >= 0);
		close(fd);
		m_continue.wait();
		/* remove the FIFO */
		unlink(FIFO_NAME);

	}

	void wait_for_writer_ready()
	{
		m_ready.wait();
	}

	void signal_continue()
	{
		m_continue.set();
	}

	int64_t get_tid()
	{
		return m_tid;
	}

private:
	Poco::Event m_ready;
	Poco::Event m_continue;
	int64_t m_tid;
};

class named_pipe_reader
{
public:
	named_pipe_reader()
	{
		m_tid = 0;
	}

	void run()
	{
		int fd;
		char buf[MAX_BUF];
		m_tid = syscall(SYS_gettid);
		m_ready.set();
		/* open, read, and display the message from the FIFO */
		fd = open(FIFO_NAME, O_RDONLY);
		ASSERT_TRUE(read(fd, buf, MAX_BUF) >= 0);

		close(fd);
	}

	void signal_continue()
	{
		m_continue.set();
	}

	void wait_for_reader_ready()
	{
		m_ready.wait();
	}

	int64_t get_tid()
	{
		return m_tid;
	}
private:
	Poco::Event m_ready;
	Poco::Event m_continue;
	int64_t m_tid;
};

TEST_F(sys_call_test, DISABLED_named_pipe)
{
	Poco::Thread writer_thread;
	Poco::Thread reader_thread;
	named_pipe_writer writer;
	named_pipe_reader reader;
	Poco::RunnableAdapter<named_pipe_writer> writer_runnable(writer, &named_pipe_writer::run);
	Poco::RunnableAdapter<named_pipe_reader> reader_runnable(reader, &named_pipe_reader::run);

	//
	// FILTER
	//
	event_filter_t filter = [&](sinsp_evt * evt)
	{
		return evt->get_tid() == writer.get_tid() || evt->get_tid() == reader.get_tid();
	};

	//
	// INITIALIZATION
	//
	run_callback_t test = [&](sinsp* inspector)
	{
		writer_thread.start(writer_runnable);
		writer.wait_for_writer_ready();
		reader_thread.start(reader_runnable);
		reader.wait_for_reader_ready();
		reader_thread.join();

		writer.signal_continue();
		writer_thread.join();
	};

	//
	// OUTPUT VALIDATION
	//
	captured_event_callback_t callback = [&](const callback_param& param)
	{
	};


	ASSERT_NO_FATAL_FAILURE({event_capture::run(test, callback, filter);});

}