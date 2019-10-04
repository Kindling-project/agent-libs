#pragma once

#include "draios.proto.h"

// in order to override any of the generated function,
// 1) create the impl class that derives from the generated aggregator
// 2) write the allocator, and override any functions you need to
// 3) override the appropriate builder function in the builder_impl below


class process_details_message_aggregator_impl : public process_details_message_aggregator
{
public:
	process_details_message_aggregator_impl(const message_aggregator_builder& builder)
		: process_details_message_aggregator(builder)
	{}

private:
	// args need to be treated like a list, not a set, so needs special handling
	virtual void aggregate_args(const draiosproto::process_details& input,
				    draiosproto::process_details& output);

	// backend always sets container id, even if not set in input, so we do, too
	virtual void aggregate_container_id(const draiosproto::process_details& input,
					    draiosproto::process_details& output);
};

class metrics_message_aggregator_impl : public metrics_message_aggregator
{
public:
	metrics_message_aggregator_impl(const message_aggregator_builder& builder)
		: metrics_message_aggregator(builder)
	{}

public:
    // we have an awkward dependency that the BE depends on a hash of the program. This
    // computes that hash, which is effectively the java hash of the equivalent objects
    static int32_t java_string_hash(const std::string& input, uint32_t end_pos = UINT32_MAX )
    {
	int32_t hash = 0;

	if (end_pos > input.size())
	{
	    end_pos = input.size();
	}

	for (uint32_t i = 0; i < end_pos; ++i)
	{
	    hash = 31 * hash + input[i];
	}
	return hash;
    }
    static int32_t java_list_hash(const google::protobuf::RepeatedPtrField<std::string>& input)
    {
	int32_t hash = 1;
	for (auto i : input)
	{
	    hash = 31 * hash + java_string_hash(i);
	}
	return hash;
    }
    static size_t program_java_hasher(const draiosproto::program& input)
    {
	const draiosproto::process& proc = input.procinfo();
	const draiosproto::process_details& details = proc.details();

	int32_t hash = 0;

	auto separator_loc = details.exe().find(": ");
	hash += java_string_hash(details.exe(),
				 separator_loc == std::string::npos ? details.exe().size() : separator_loc);
	hash = 31 * hash + java_list_hash(details.args());
	hash += java_string_hash(details.container_id());
	hash += java_string_hash(input.environment_hash());

	return hash;
    }

private:
    // mapping from pid to hash. This is used to give us a PID agnostic ID of a process.
    // We store the mapping here until reset
    std::map<uint32_t, size_t> pid_map;

    // we have to ensure we've populated the pid_map before anyone uses it. this tracks
    // whether we've aggregated programs yet. Reset with each call to aggregate()
    bool aggregated_programs;

    // Overrides for program which ensures pid_map is populated
    virtual void aggregate_programs(const draiosproto::metrics& input,
				    draiosproto::metrics& output);

    // two fields that depend on the pid_map and must substitute pid for value
    // stored in pid_map
    virtual void aggregate_ipv4_connections(const draiosproto::metrics& input,
					    draiosproto::metrics& output);
    virtual void aggregate_ipv4_incomplete_connections_v2(const draiosproto::metrics& input,
							  draiosproto::metrics& output);

    // need to reset the pid_map field
    virtual void reset()
    {
	pid_map.clear();
	metrics_message_aggregator::reset();
    }

    virtual void aggregate(const draiosproto::metrics& input, draiosproto::metrics& output);

    friend class test_helper;
};

// for any message type which we've overridden, we have to override it's builder
// function as well
class message_aggregator_builder_impl : public message_aggregator_builder
{
public:
	virtual agent_message_aggregator<draiosproto::process_details>& build_process_details() const;
	virtual agent_message_aggregator<draiosproto::metrics>& build_metrics() const;
};
