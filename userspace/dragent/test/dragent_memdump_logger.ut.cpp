/**
 * @file
 *
 * Unit tests for dragent_memdump_logger
 *
 * @copyright Copyright (c) 2019 Sysdig Inc., All Rights Reserved
 */
#include "dragent_memdump_logger.h"
#include "infra_event_sink.h"
#include "user_event.h"
#include <gtest.h>
#include <limits>
#include <string>

namespace
{

/**
 * Dummy realization of the dragent::infra_event_sink that just saves copies
 * of the values passed to push_infra_event.  This will enable the unit tests
 * to verify that dragent_memdump_logger dispatches the logs to the
 * infra_event_sink.
 */
class test_infra_sink : public dragent::infra_event_sink
{
public:
	const static std::string DEFAULT_STRING_VALUE;
	const static uint64_t DEFAULT_UINT_VALUE;

	/**
	 * Initializes all fields to default values.
	 */
	test_infra_sink():
		m_ts(DEFAULT_UINT_VALUE),
		m_tid(DEFAULT_UINT_VALUE),
		m_source(DEFAULT_STRING_VALUE),
		m_name(DEFAULT_STRING_VALUE),
		m_description(DEFAULT_STRING_VALUE),
		m_scope(DEFAULT_STRING_VALUE)
	{ }

	/**
	 * Called by the dragent_memdump_logger when it receives a
	 * properly-formatted log.
	 */
	void push_infra_event(const uint64_t ts,
	                      const uint64_t tid,
	                      const std::string& source,
	                      const std::string& name,
	                      const std::string& description,
	                      const std::string& scope) override
	{
		m_ts = ts;
		m_tid = tid;
		m_source = source;
		m_name = name;
		m_description = description;
		m_scope = scope;
	}

	uint64_t m_ts;
	uint64_t m_tid;
	std::string m_source;
	std::string m_name;
	std::string m_description;
	std::string m_scope;
};
const std::string test_infra_sink::DEFAULT_STRING_VALUE = "--UNSET--";
const uint64_t test_infra_sink::DEFAULT_UINT_VALUE = std::numeric_limits<uint64_t>::max();

} // end namespace

/**
 * Ensure that a properly-formatted sent to a dragent_memdump_logger
 * gets passed to the associated infra_event_sink.
 */
TEST(dragent_memdump_logger_test, valid_log)
{
	const std::string source = "source";
	test_infra_sink sink;
	dragent_memdump_logger logger(&sink);

	sinsp_user_event evt(
		time(nullptr),
		"some name",
		"some description",
		"some scope",
		{},
		sinsp_user_event::UNKNOWN_SEVERITY);

	logger.log(source, evt);

	ASSERT_NE(sink.m_ts, test_infra_sink::DEFAULT_UINT_VALUE);
	ASSERT_EQ(0, sink.m_tid);
	ASSERT_EQ(source, sink.m_source);
	ASSERT_EQ("some name", sink.m_name);
	ASSERT_EQ("some description", sink.m_description);
	ASSERT_EQ("some scope", sink.m_scope);
}

/**
 * Ensure that if the infra_event_sink given to the dragent_memdump_logger
 * is nullptr, logs are silently dropped.
 */
TEST(dragent_memdump_logger_test, null_sink_does_nothing)
{
	dragent_memdump_logger logger(nullptr);

	sinsp_user_event evt(
		time(nullptr),
		"some name",
		"some description",
		"some scope",
		{},
		sinsp_user_event::UNKNOWN_SEVERITY);

	// This shouldn't crash the program :)
	ASSERT_NO_FATAL_FAILURE(logger.log("source", evt));
}