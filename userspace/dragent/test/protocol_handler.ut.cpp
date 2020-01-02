#include <gtest.h>
#include "protocol_handler.h"
#include "protocol.h"

TEST(protocol_handler_test, config)
{
	std::string config = R"(
protobuf_print: true
compression:
  enabled: false
audit_tap:
  debug_only: false
)";
	yaml_configuration config_yaml(config);
        ASSERT_EQ(0, config_yaml.errors().size());

	protocol_handler::c_print_protobuf.init(config_yaml);
	protocol_handler::c_audit_tap_debug_only.init(config_yaml);

	ASSERT_EQ(protocol_handler::c_print_protobuf.get_value(), true);
	ASSERT_EQ(protocol_handler::c_audit_tap_debug_only.get_value(), false);
}

TEST(protocol_handler_test, flush_interval)
{
	// we won't use this, but we need it since some of the other APIs still do it the
	// "wrong" way so we need the queue
	protocol_queue input_queue(10);

	protocol_handler ph(input_queue);

	auto compressor = null_protobuf_compressor::get();

	auto metrics = std::make_shared<draiosproto::metrics>();
	auto i = ph.handle_uncompressed_sample(1, metrics, 2, compressor);
	EXPECT_EQ(i->flush_interval, 2);
}