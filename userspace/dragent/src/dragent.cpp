#include "app_checks_proxy.h"
#include "async_aggregator.h"
#include "avoid_block_channel.h"
#include "capture_job_handler.h"
#include "command_line_manager.h"
#include "command_line_request_message_handler.h"
#include "command_line_runnable.h"
#include "common_logger.h"
#include "config_data_message_handler.h"
#include "config_data_rest_request_handler.h"
#include "config_rest_request_handler.h"
#include "configlist_rest_request_handler.h"
#include "configuration.h"
#include "configuration_manager.h"
#include "connection_manager.h"
#include "container_size_requestor.h"
#include "crash_handler.h"
#include "dragent.h"
#include "dragent_memdump_logger.h"
#include "dragent_user_event_callback.h"
#include "dump_request_start_message_handler.h"
#include "dump_request_stop_message_handler.h"
#include "error_handler.h"
#include "exit_code.h"
#include "fault_rest_request_handler.h"
#include "faultlist_rest_request_handler.h"
#include "file_rest_request_handler.h"
#include "globally_readable_file_channel.h"
#include "handshake_helpers.h"
#include "memdump_logger.h"
#include "metric_serializer.h"
#include "monitor.h"
#include "null_message_handler.h"
#include "post_aggregated_metrics_rest_request_handler.h"
#include "pre_aggregated_metrics_rest_request_handler.h"
#include "process_helpers.h"
#include "procfs_parser.h"
#include "promscrape.h"
#include "promscrape_proxy.h"
#include "protobuf_compression.h"
#include "protobuf_metric_serializer.h"
#include "rest_request_handler_factory.h"
#include "rest_server.h"
#include "running_state.h"
#include "security_compliance_calendar_message_handler.h"
#include "security_compliance_run_message_handler.h"
#include "security_orchestrator_events_message_handler.h"
#include "security_policies_v2_message_handler.h"
#include "sinsp_factory.h"
#include "sinsp_worker.h"
#include "statsd_server.h"
#include "statsite_config.h"
#include "statsite_forwarder.h"
#include "type_config.h"
#include "user_event_channel.h"
#include "utils.h"
#include "webpage_rest_request_handler.h"
#include "library_configs.h"

#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <time.h>

#ifndef CYGWING_AGENT
#include <gperftools/malloc_extension.h>
#include <grpc/support/log.h>
#include <sched.h>
#else
#include "windows_helpers.h"
#endif

using namespace std;
using namespace dragent;

// local helper functions
namespace
{
COMMON_LOGGER();

type_config<bool> c_use_statsite_forwarder(
    true,
    "Use statsite_forwarder instead of system call trace for container statsd metrics",
    "statsd",
    "use_forwarder");

type_config<bool> c_sdagent_app_checks_python_26_supported(false,
                                                           "sdagent check the python 2.6 support",
                                                           "app_checks_python_26_supported");

type_config<bool>::ptr c_rest_feature_flag =
    type_config_builder<bool>(false,
                              "Feature flag to turn on the REST server.",
                              "feature_flag_rest_server")
        .hidden()
        .mutable_only_in_internal_build()
        .build();

type_config<uint16_t>::ptr c_rest_port =
    type_config_builder<uint16_t>(24482,
                                  "TCP port on which the Agent REST server listens for connections",
                                  "rest_server",
                                  "tcp_port")
        .hidden()  // Hidden until feature is released
        .build();

type_config<uint64_t> c_serializer_timeout_s(10,
                                             "Watchdog timeout for the serializer thread",
                                             "serializer_timeout");

type_config<bool> c_app_check_thread(true,
                                     "Run a dedicated thread for app_check processing",
                                     "app_check_thread");

type_config<uint64_t> c_app_check_timeout_s(60,
                                            "Watchdog timeout for the app_check thread",
                                            "app_check_timeout");

type_config<bool> c_promscrape_thread(true,
                                      "Run a dedicated thread for promscrape processing",
                                      "promscrape_thread");

type_config<uint64_t> c_promscrape_timeout_s(60,
                                             "Watchdog timeout for the promscrape thread",
                                             "promscrape_thread_timeout");

type_config<bool> c_10s_flush_enabled(false,
                                      "Enable agent-side aggregation",
                                      "10s_flush_enable");

type_config<std::string> c_promscrape_labels("--source.label=pod_id,sysdig_k8s_pod_uid,remove --source.label=container_name,sysdig_k8s_pod_container_name,remove --source.label=sysdig_bypass,sysdig_bypass,remove --source.label=sysdig_omit_source,sysdig_omit_source,remove",
                                             "source labels for promscrape to attach to results",
                                             "promscrape_labels");

type_config<uint64_t> c_watchdog_max_memory_usage_mb(1024,
                                                     "maximum memory usage for watchdog",
                                                     "watchdog",
                                                     "max_memory_usage_mb");

type_config<uint64_t> c_watchdog_warn_memory_usage_mb(256,
                                                      "warn memory usage for watchdog",
                                                      "watchdog",
                                                      "warn_memory_usage_mb");

type_config<std::vector<std::string>> c_log_file_component_overrides(
					{},
					"Component level overrides to global log level",
					"log",
					"file_priority_by_component");

type_config<uint64_t>::ptr c_wait_before_ready_sec =
	type_config_builder<uint64_t>(
		0,
		"after cointerface is ready, wait this amount of seconds before k8s readiness probe switches to ready."
		"If set to zero, cointerface is not taken into account for readiness",
		"k8s_wait_before_ready"
		).build();

string compute_sha1_digest(SHA1Engine& engine, const string& path)
{
	engine.reset();
	ifstream fs(path);
	char readbuf[4096];

	while (fs.good())
	{
		fs.read(readbuf, sizeof(readbuf));
		engine.update(readbuf, fs.gcount());
	}

	return DigestEngine::digestToHex(engine.digest());
}

// Number of seconds (of uptime) after which to update the priority of the
// processes. This was chosen arbitrarily to be after the processes had time
// to start.
const uint32_t TIME_TO_UPDATE_PROCESS_PRIORITY = 5;

static void g_signal_callback(int sig)
{
	running_state::instance().shut_down();
}

static void g_usr_signal_callback(int sig)
{
	dragent_configuration::m_signal_dump = true;
}

static void g_usr2_signal_callback(int sig)
{
	dragent_configuration::m_send_log_report = true;
}

static void g_trace_signal_callback(int sig)
{
	dragent_configuration::m_enable_trace = true;
}

std::unique_ptr<librest::rest_server> s_rest_server;

/**
 * Enable the REST server (if enabled); otherwise, do nothing.
 */
void enable_rest_server(dragent_configuration& configuration)
{
	if (!c_rest_feature_flag->get_value())
	{
		return;
	}

	if (s_rest_server)
	{
		return;
	}

	Poco::SharedPtr<librest::rest_request_handler_factory> factory(
	    new librest::rest_request_handler_factory());

	// Register path handlers with the factory...
	factory->register_path_handler<configlist_rest_request_handler>();
	factory->register_path_handler<config_rest_request_handler>();
	factory->register_path_handler<post_aggregated_metrics_rest_request_handler>();
	factory->register_path_handler<pre_aggregated_metrics_rest_request_handler>();
	factory->register_path_handler<config_data_rest_request_handler>();
	factory->register_path_handler<webpage_rest_request_handler>();
	factory->register_path_handler<file_rest_request_handler>();
#if defined(FAULT_INJECTION_ENABLED)
	factory->register_path_handler<faultlist_rest_request_handler>();
	factory->register_path_handler<fault_rest_request_handler>();
#endif  // defined(FAULT_INJECTION_ENABLED)

	config_data_rest_request_handler::set_config_data_message_handler(
	    std::make_shared<config_data_message_handler>(configuration));

	s_rest_server = make_unique<librest::rest_server>(factory, c_rest_port->get_value());
	s_rest_server->start();
}

/**
 * Disable the REST server (if enabled); otherwise, do nothing.
 */
void disable_rest_server()
{
	if (s_rest_server.get() == nullptr)
	{
		return;
	}

	s_rest_server->stop();
	s_rest_server.reset();
}

}  // end namespace

dragent_app::dragent_app()
    : m_help_requested(false),
      m_version_requested(false),
#ifdef CYGWING_AGENT
      m_windows_service_parent(false),
#endif
#ifndef CYGWING_AGENT
      m_unshare_ipcns(true),
#endif
      m_aggregator_queue(MAX_SAMPLE_STORE_SIZE),
      m_serializer_queue(MAX_SAMPLE_STORE_SIZE),
      m_transmit_queue(MAX_SAMPLE_STORE_SIZE),
      m_internal_metrics(std::make_shared<internal_metrics>()),
      m_protocol_handler(m_transmit_queue),
      m_capture_job_handler(&m_configuration, &m_transmit_queue),
      m_sinsp_worker(&m_configuration,
                     m_internal_metrics,
                     m_protocol_handler,
                     &m_capture_job_handler),
      m_log_reporter(m_protocol_handler, &m_configuration),
      m_subprocesses_logger(&m_configuration, &m_log_reporter, m_transmit_queue),
      m_cointerface_ready(false),
      m_last_dump_s(0)
{
}

dragent_app::~dragent_app()
{
	std::shared_ptr<config_data_message_handler> ptr;

	config_data_rest_request_handler::set_config_data_message_handler(ptr);
	google::protobuf::ShutdownProtobufLibrary();
}

void dragent_app::initialize(Application& self)
{
	ServerApplication::initialize(self);
}

void dragent_app::uninitialize()
{
	ServerApplication::uninitialize();
}

void dragent_app::defineOptions(OptionSet& options)
{
	ServerApplication::defineOptions(options);

	options.addOption(Option("help", "h", "display help information on command line arguments")
	                      .required(false)
	                      .repeatable(false));

	options.addOption(
	    Option("configtest", "t", "test config file and exit.").required(false).repeatable(false));

	options.addOption(Option("consolepriority",
	                         "",
	                         "min priority of the log messages that go on console. Can be 'error', "
	                         "'warning', 'info' or 'debug'.")
	                      .required(false)
	                      .repeatable(false)
	                      .argument("priority"));

	options.addOption(Option("filepriority",
	                         "",
	                         "min priority of the log messages that go on file. Can be 'error', "
	                         "'warning', 'info' or 'debug'.")
	                      .required(false)
	                      .repeatable(false)
	                      .argument("priority"));

	options.addOption(Option("readfile", "r", "file to open.")
	                      .required(false)
	                      .repeatable(false)
	                      .argument("filename"));

	options.addOption(Option("evtcount", "c", "numer of events after which the capture stops.")
	                      .required(false)
	                      .repeatable(false)
	                      .argument("count"));

	options.addOption(Option("customerid", "i", "force the customer id.")
	                      .required(false)
	                      .repeatable(false)
	                      .argument("id"));

	options.addOption(Option("srvaddr", "", "the address of the server to connect to.")
	                      .required(false)
	                      .repeatable(false)
	                      .argument("address"));

	options.addOption(Option("srvport", "", "the TCP port to use.")
	                      .required(false)
	                      .repeatable(false)
	                      .argument("port"));

#ifndef CYGWING_AGENT
	options.addOption(Option("noipcns", "", "keep IPC namespace (for internal use)")
	                      .required(false)
	                      .repeatable(false));
#endif

	options.addOption(Option("dragentpid", "", "pid file.")
	                      .required(false)
	                      .repeatable(false)
	                      .argument("dragentpid"));

	options.addOption(Option("version", "v", "display version").required(false).repeatable(false));

#ifdef CYGWING_AGENT
	options.addOption(
	    Option("serviceparent",
	           "",
	           "assume we are run by a windows service and listen to the service event.")
	        .required(false)
	        .repeatable(false));
#endif
}

void dragent_app::handleOption(const std::string& name, const std::string& value)
{
	ServerApplication::handleOption(name, value);

	if (name == "help")
	{
		m_help_requested = true;
	}
	else if (name == "consolepriority")
	{
		m_configuration.m_min_console_priority = dragent_configuration::string_to_priority(value);
	}
	else if (name == "filepriority")
	{
		m_configuration.m_min_file_priority = dragent_configuration::string_to_priority(value);
	}
	else if (name == "readfile")
	{
		m_configuration.m_input_filename = value;
	}
	else if (name == "evtcount")
	{
		m_configuration.m_evtcnt = NumberParser::parse64(value);
	}
	else if (name == "customerid")
	{
		m_configuration.m_customer_id = value;
	}
	else if (name == "srvaddr")
	{
		m_configuration.m_server_addr = value;
	}
	else if (name == "srvport")
	{
		m_configuration.m_server_port = (uint16_t)NumberParser::parse(value);
	}
#ifndef CYGWING_AGENT
	else if (name == "noipcns")
	{
		m_unshare_ipcns = false;
	}
#endif
	else if (name == "dragentpid")
	{
		m_pidfile = value;
	}
	else if (name == "version")
	{
		m_version_requested = true;
	}
#ifdef CYGWING_AGENT
	else if (name == "serviceparent")
	{
		m_windows_service_parent = true;
	}
#endif
}

void dragent_app::displayHelp()
{
	HelpFormatter helpFormatter(options());
	helpFormatter.setCommand(commandName());
	helpFormatter.setUsage("OPTIONS");
	helpFormatter.setHeader("Draios Agent.");
	helpFormatter.format(std::cout);
}

#ifndef CYGWING_AGENT
static void dragent_gpr_log(gpr_log_func_args* args)
{
	// If logging hasn't been set up yet, skip the message. Add an
	// ASSSERT so we'll notice for dev builds, though.
	ostringstream os;

	if (!g_log)
	{
		ASSERT(false);
		return;
	}

	os << "GRPC: [" << args->file << ":" << args->line << "] " << args->message;

	switch (args->severity)
	{
	case GPR_LOG_SEVERITY_DEBUG:
		LOG_DEBUG(os.str());
		break;
	case GPR_LOG_SEVERITY_INFO:
		LOG_INFO(os.str());
		break;
	case GPR_LOG_SEVERITY_ERROR:
		LOG_ERROR(os.str());
		break;
	default:
		LOG_DEBUG(os.str());
		break;
	}
}
#endif

int dragent_app::main(const std::vector<std::string>& args)
{
	if (m_help_requested)
	{
		displayHelp();
		return Application::EXIT_OK;
	}

	if (m_version_requested)
	{
		printf(AGENT_VERSION "\n");
		return Application::EXIT_OK;
	}

	command_line_manager::command_info cmd;
	cmd.permissions = {CLI_AGENT_STATUS};
	cmd.short_description = "The version of the Sysdig Agent.";
	cmd.handler = [](const command_line_manager::argument_list &args) { return AGENT_VERSION;};
	command_line_manager::instance().register_command("version", cmd);

//
// Set up logging with grpc.
//
#ifndef CYGWING_AGENT
	gpr_set_log_function(dragent_gpr_log);
#endif

	//
	// Make sure the agent never creates world-writable files
	//
	umask(0027);

//
// Never move this further down!
// It's important that the pidfile gets created immediately!
//
#ifndef CYGWING_AGENT
	string me = config().getString("application.path", CMAKE_INSTALL_PREFIX "/bin/dragent");
	monitor monitor_process(m_pidfile, move(me), {"--noipcns"});
#else
	monitor monitor_process(m_pidfile, m_windows_service_parent);
#endif

	try
	{
		m_configuration.init(this, true);
	}
	catch (const yaml_configuration_exception& ex)
	{
		std::cerr << "Failed to init sinsp_worker. Exception message: " << ex.what() << '\n';
		running_state::instance().shut_down();
	}

	m_had_unclean_shutdown = remove_file_if_exists(m_configuration.m_log_dir, K8S_PROBE_FILE);

	// superhack: the old driver mode config doesn't play nice with the feature manager. So
	// we perform a translation here.
	if (m_configuration.m_mode == dragent_mode_t::STANDARD)
	{
		configuration_manager::instance().get_mutable_config<bool>("feature.driver")->set(true);
		configuration_manager::instance()
		    .get_mutable_config<bool>("feature.full_syscalls")
		    ->set(true);
	}
	if (m_configuration.m_mode == dragent_mode_t::NODRIVER)
	{
		configuration_manager::instance().get_mutable_config<bool>("feature.driver")->set(false);
		configuration_manager::instance()
		    .get_mutable_config<bool>("feature.full_syscalls")
		    ->set(false);
	}
	if (m_configuration.m_mode == dragent_mode_t::SIMPLEDRIVER)
	{
		configuration_manager::instance().get_mutable_config<bool>("feature.driver")->set(true);
		configuration_manager::instance()
		    .get_mutable_config<bool>("feature.full_syscalls")
		    ->set(false);
	}

	if (m_configuration.m_mode_explicitly_set)
	{
		configuration_manager::instance().get_mutable_config<bool>("feature.driver")->set_set_in_config(true);
		configuration_manager::instance().get_mutable_config<bool>("feature.full_syscalls")->set_set_in_config(true);
	}

	// Ensure the feature manager has validatead the config
	if (!feature_manager::instance().initialize())
	{
		std::cerr << "Failed to init features." << '\n';
		running_state::instance().shut_down();
	}

	// superhack: prom tries to manage it's own enablement, which is not yet fully integrated
	// with the feature manager. As such, if the feature manager determines it's disabled,
	// we need to ensure that gets forwarded to the prom feature
	m_configuration.m_prom_conf.set_enabled(feature_manager::instance().get_enabled(PROMETHEUS));

#ifndef _WIN32
	//
	// Before running the monitor, unblock all the signals,
	// because dragent might be restarted from a Poco thread (e.g.
	// during auto-update), and the Poco implementation blocks
	// signals by default in threads in order to allow a deterministic
	// signal recipient instead of a random one.
	//
	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGQUIT);
	sigaddset(&sigs, SIGTERM);
	sigaddset(&sigs, SIGPIPE);
	sigprocmask(SIG_UNBLOCK, &sigs, NULL);

	// Ignore SIGSTKFLT. It's used to enable tracing (sent from watchdog to sinsp_worker)
	// but the default action is to kill the process. Ignore this signal before setting
	// a handler in sinsp_worker so that `killall -STKFLT dragent` can be used for testing
	// and for manually enabling tracing
	signal(SIGSTKFLT, SIG_IGN);

#ifndef CYGWING_AGENT
	if (m_unshare_ipcns && unshare(CLONE_NEWIPC) < 0)
	{
		std::cerr << "Cannot create private IPC namespace: " << strerror(errno) << '\n';
	}
#endif

	struct rlimit msgqueue_rlimits = {.rlim_cur = m_configuration.m_rlimit_msgqueue,
	                                  .rlim_max = m_configuration.m_rlimit_msgqueue};
	if (setrlimit(RLIMIT_MSGQUEUE, &msgqueue_rlimits) != 0)
	{
		std::cerr << "Cannot set msgqueue limits: " << strerror(errno) << '\n';
	}

	process_helpers::subprocess_cpu_cgroup default_cpu_cgroup("/default",
	                                                          c_default_cpu_shares.get_value(),
	                                                          c_default_cpu_quota.get_value());
	default_cpu_cgroup.create();

	process_helpers::subprocess_cpu_cgroup cointerface_cpu_cgroup(
	    "/cointerface",
	    c_cointerface_cpu_shares.get_value(),
	    c_cointerface_cpu_quota.get_value());
	cointerface_cpu_cgroup.create();

	// Sanity check some prometheus and promscrape configs. May change some configs.
	// Making sure to do it before sdagent_main() which prints the configuration
	m_configuration.m_prom_conf.validate_config(m_configuration.c_root_dir.get_value());

	// Add our main process
	monitor_process.emplace_process(
	    "sdagent",
	    [=]() {
		    default_cpu_cgroup.enter();

		    // only set to get agent show in the watchdog loop
		    m_subprocesses_state["sdagent"].set_name("sdagent");

		    // Unlike the other processes, the agent itself
		    // doesn't get a pid from the log file. So set it
		    // here.
		    m_subprocesses_state["sdagent"].reset(getpid(), 0, 0);

		    struct sigaction sa;
		    memset(&sa, 0, sizeof(sa));
		    sigemptyset(&sa.sa_mask);
		    sa.sa_handler = g_signal_callback;

		    sigaction(SIGINT, &sa, NULL);
		    sigaction(SIGQUIT, &sa, NULL);
		    sigaction(SIGTERM, &sa, NULL);

		    sa.sa_handler = g_usr_signal_callback;
		    sigaction(SIGUSR1, &sa, NULL);
		    sa.sa_handler = g_usr2_signal_callback;
		    sigaction(SIGUSR2, &sa, NULL);
		    sa.sa_handler = g_trace_signal_callback;
		    sigaction(SIGSTKFLT, &sa, NULL);

		    if (crash_handler::initialize() == false)
		    {
			    ASSERT(false);
		    }

		    return this->sdagent_main();
	    },
	    true);

	if (m_configuration.java_present() && feature_manager::instance().get_enabled(JMX) &&
	    getpid() != 1)
	{
		m_jmx_pipes = make_unique<errpipe_manager>();
		auto* state = &m_subprocesses_state["sdjagent"];
		state->set_name("sdjagent");
		m_subprocesses_logger.add_logfd(m_jmx_pipes->get_file(), sdjagent_parser(), state);

		monitor_process.emplace_process("sdjagent", [=]() -> int {
			default_cpu_cgroup.enter();
			static const auto MAX_SDJAGENT_ARGS = 50;
			this->m_jmx_pipes->attach_child();

			// Our option parser is pretty simple, for example an arg with spaces inside
			// double quotes will not work, eg:
			// -Xmx:myamazingconfig="test with spaces" -Xmx256m
			auto sdjagent_opts_split = sinsp_split(m_configuration.m_sdjagent_opts, ' ');

			const char* args[MAX_SDJAGENT_ARGS];
			unsigned j = 0;
			args[j++] = "java";
			for (const auto& opt : sdjagent_opts_split)
			{
				args[j++] = opt.c_str();
			}

			const string java_library_path =
			    string("-Djava.library.path=") + m_configuration.c_root_dir.get_value() + "/lib";
			args[j++] = java_library_path.c_str();
			args[j++] = "-Dsun.rmi.transport.connectionTimeout=" SDJAGENT_JMX_TIMEOUT;
			args[j++] = "-Dsun.rmi.transport.tcp.handshakeTimeout=" SDJAGENT_JMX_TIMEOUT;
			args[j++] = "-Dsun.rmi.transport.tcp.responseTimeout=" SDJAGENT_JMX_TIMEOUT;
			args[j++] = "-Dsun.rmi.transport.tcp.readTimeout=" SDJAGENT_JMX_TIMEOUT;
			args[j++] = "-jar";
			File sdjagent_jar(m_configuration.c_root_dir.get_value() + "/share/sdjagent.jar");

			std::string jar_file =
			    sdjagent_jar.exists()
			        ? (m_configuration.c_root_dir.get_value() + "/share/sdjagent.jar")
			        : "../sdjagent/java/sdjagent-1.0-jar-with-dependencies.jar";

			args[j++] = jar_file.c_str();
			args[j++] = NULL;

			execv(this->m_configuration.m_java_binary.c_str(), (char* const*)args);

			std::cerr << "{ \"pid\": 0, \"level\": \"SEVERE\", \"message\": \"Cannot load "
			             "sdjagent, errno: "
			          << errno << "\" }" << std::endl;
			return (EXIT_FAILURE);
		});
	}

	// Configure statsite subprocess
	if (feature_manager::instance().get_enabled(STATSD))
	{
		const std::string statsite_ini =
#ifndef CYGWING_AGENT
		    m_configuration.c_root_dir.get_value() + "/etc/statsite.ini";
#else
		    "statsite.ini";
#endif

		libsanalyzer::statsite_config::instance().write_statsite_configuration(
		    statsite_ini,
		    m_configuration.m_raw_file_priority,
		    m_configuration.m_percentiles);

		m_statsite_pipes = make_shared<pipe_manager>();
		m_subprocesses_logger.add_logfd(m_statsite_pipes->get_err_fd(), [this](const string& data) {
			if (data.find("Failed to bind") != string::npos)
			{
				this->m_sinsp_worker.set_statsd_capture_localhost(true);
			}
			// statsite logs does not have info about level, use error if keyword `Failed` is inside
			// or use
			// information
			if (data.find("Failed") != string::npos)
			{
				LOG_ERROR(data);
			}
			else
			{
				LOG_INFO(data);
			}
		});

		monitor_process.emplace_process("statsite", [=]() -> int {
			default_cpu_cgroup.enter();
			this->m_statsite_pipes->attach_child_stdio();
#ifndef CYGWING_AGENT
			execl((m_configuration.c_root_dir.get_value() + "/bin/statsite").c_str(),
			      "statsite",
			      "-f",
			      statsite_ini.c_str(),
			      (char*)NULL);
#else
				execl("../../../../dependencies/statsite-private-0.7.0-sysdig3/statsite",
				      "statsite",
				      "-f",
				      statsite_ini.c_str(),
				      (char*)NULL);
#endif
			return (EXIT_FAILURE);
		});

		if (c_use_statsite_forwarder.get_value() ||
		    (m_configuration.m_mode == dragent_mode_t::NODRIVER))
		{
			m_statsite_forwarder_pipe = make_unique<errpipe_manager>();
			auto state = &m_subprocesses_state["statsite_forwarder"];
			state->set_name("statsite_forwarder");
			m_subprocesses_logger.add_logfd(m_statsite_forwarder_pipe->get_file(),
			                                sinsp_logger_parser("statsite_forwarder"),
			                                state);
			monitor_process.emplace_process("statsite_forwarder", [=]() -> int {
				default_cpu_cgroup.enter();
				m_statsite_forwarder_pipe->attach_child();
				g_logger.add_stderr_log();
				statsite_forwarder fwd(this->m_statsite_pipes->get_io_fds(),
				                       libsanalyzer::statsite_config::instance().get_udp_port());
				return fwd.run();
			});
		}
	}

#ifndef CYGWING_AGENT
	if (m_configuration.python_present() && (feature_manager::instance().get_enabled(APP_CHECKS) ||
	                                         m_configuration.m_prom_conf.enabled()))
	{
		m_sdchecks_pipes = make_unique<errpipe_manager>();
		auto state = &m_subprocesses_state["sdchecks"];
		state->set_name("sdchecks");
		m_subprocesses_logger.add_logfd(m_sdchecks_pipes->get_file(), sdchecks_parser(), state);
		if (!c_sdagent_app_checks_python_26_supported.get_value() &&
		    m_configuration.check_python_version26())
		{
			// LOG_ERROR can't be used this early in startup and that's why cerr is being used.
			std::cerr << "Error : Python 2.6 is not a supported environment for App Checks. "
			             "Please upgrade to Python 2.7. "
			             "Contact Sysdig Support for additional help."
			          << std::endl;

			bool success = feature_manager::instance().deprecated_disable(APP_CHECKS);
			if (!success)
			{
				std::cerr << "Error: Cannot disable App Checks.\n";
				return EXIT_FAILURE;
			}
		}
		else
		{
			monitor_process.emplace_process("sdchecks", [=]() {
				default_cpu_cgroup.enter();
				this->m_sdchecks_pipes->attach_child();

				setenv("LD_LIBRARY_PATH",
				       (m_configuration.c_root_dir.get_value() + "/lib").c_str(),
				       1);
				const char* python = this->m_configuration.m_python_binary.c_str();
				execl(python,
				      python,
				      (m_configuration.c_root_dir.get_value() + "/bin/sdchecks").c_str(),
				      "run",
				      NULL);

				return (EXIT_FAILURE);
			});
		}
	}
	if (m_configuration.m_system_supports_containers)
	{
		m_mounted_fs_reader_pipe = make_unique<errpipe_manager>();
		auto* state = &m_subprocesses_state["mountedfs_reader"];
		state->set_name("mountedfs_reader");
		m_subprocesses_logger.add_logfd(m_mounted_fs_reader_pipe->get_file(),
		                                sinsp_encoded_parser("mountedfs_reader"),
		                                state);
		monitor_process.emplace_process("mountedfs_reader", [=]() {
			default_cpu_cgroup.enter();
			m_mounted_fs_reader_pipe->attach_child();
			auto sev = static_cast<sinsp_logger::severity>(
			    std::max(this->m_configuration.m_min_file_priority,
			             this->m_configuration.m_min_console_priority));
			g_logger.set_severity(sev);
			g_logger.add_encoded_severity();
			g_logger.disable_timestamps();
			g_logger.add_stderr_log();

			mounted_fs_reader proc(this->m_configuration.m_remotefs_enabled,
			                       this->m_configuration.m_mounts_filter,
			                       this->m_configuration.m_mounts_limit_size,
			                       this->m_configuration.m_log_dir);
			return proc.run();
		});
	}
#endif
	if (feature_manager::instance().get_enabled(COINTERFACE))
	{
		m_cointerface_pipes = make_unique<pipe_manager>();
		auto* state = &m_subprocesses_state["cointerface"];
		state->set_name("cointerface");
		m_subprocesses_logger.add_logfd(m_cointerface_pipes->get_err_fd(),
		                                cointerface_parser(),
		                                state);
		m_subprocesses_logger.add_logfd(m_cointerface_pipes->get_out_fd(),
		                                cointerface_parser(),
		                                state);
		monitor_process.emplace_process("cointerface", [=]() {
			cointerface_cpu_cgroup.enter();
			m_cointerface_pipes->attach_child_stdio();

			if (m_configuration.m_cointerface_cpu_profile_enabled)
			{
				string logfile = m_configuration.m_log_dir + "/cpu.prof";
				execl((m_configuration.c_root_dir.get_value() + "/bin/cointerface").c_str(),
				      "cointerface",
				      "-cpuprofile",
				      logfile.c_str(),
				      "-eventspertrace",
				      to_string(m_configuration.m_cointerface_events_per_profile).c_str(),
				      "-keeptraces",
				      to_string(m_configuration.m_cointerface_total_profiles).c_str(),
				      "-memprofile",
				      m_configuration.m_cointerface_mem_profile_enabled ? "true" : "false",
				      (char*)NULL);
			}
			else
			{
				execl((m_configuration.c_root_dir.get_value() + "/bin/cointerface").c_str(),
				      "cointerface",
				      (char*)NULL);
			}

			return (EXIT_FAILURE);
		});
	}
#ifndef CYGWING_AGENT
	if (m_configuration.m_promex_enabled && m_configuration.m_promex_connect_url.empty())
	{
		m_promex_pipes = make_unique<pipe_manager>();
		auto* state = &m_subprocesses_state["promex"];
		state->set_name("promex");
		m_subprocesses_logger.add_logfd(m_promex_pipes->get_out_fd(),
		                                sinsp_logger_parser("promex", true),
		                                state);
		m_subprocesses_logger.add_logfd(m_promex_pipes->get_err_fd(),
		                                sinsp_logger_parser("promex", true),
		                                state);
		monitor_process.emplace_process("promex", [=]() {
			default_cpu_cgroup.enter();
			m_promex_pipes->attach_child_stdio();

			execl((m_configuration.c_root_dir.get_value() + "/bin/promex").c_str(),
			      "promex",
			      "-prom-addr",
			      m_configuration.m_promex_url.c_str(),
			      "-container-labels",
			      m_configuration.m_promex_container_labels.c_str(),
			      (char*)NULL);

			return (EXIT_FAILURE);
		});
	}

	if (promscrape::c_use_promscrape.get_value())
	{
		m_promscrape_pipes = make_unique<pipe_manager>();
		auto* state = &m_subprocesses_state["promscrape"];
		state->set_name("promscrape");
		m_subprocesses_logger.add_logfd(m_promscrape_pipes->get_out_fd(),
		                                promscrape_parser(),
		                                state);
		m_subprocesses_logger.add_logfd(m_promscrape_pipes->get_err_fd(),
		                                promscrape_parser(),
		                                state);

		// Try to find prometheus.yaml
		std::string prom_conf_arg;
		std::vector<std::string> prompaths = {
		    m_configuration.m_default_root_dir + "/etc/prometheus.yaml",
		    m_configuration.m_default_root_dir + "/prometheus.yaml",
		    m_configuration.m_default_root_dir + "/etc/kubernetes/config/prometheus.yaml",
		    m_configuration.m_default_root_dir + (m_configuration.m_prom_conf.prom_sd() ? "/etc/prometheus-v2.default.yaml" : "/etc/prometheus-v1.default.yaml") };
		for (const auto& p : prompaths)
		{
			if (Poco::File(p).exists())
			{
				prom_conf_arg = "--config.file=" + p;
				break;
			}
		}
		monitor_process.emplace_process("promscrape", [=]() {
			string log_level = (g_logger.get_severity() >= sinsp_logger::SEV_DEBUG)
			                       ? "--log.level=debug"
			                       : "--log.level=info";
			string address = "--grpc.address=" + promscrape::c_promscrape_sock.get_value();
			default_cpu_cgroup.enter();
			m_promscrape_pipes->attach_child_stdio();
			// Promscrape v2 does service discovery, v1 lets the agent find targets
			string promscrape = m_configuration.m_prom_conf.prom_sd() ? "promscrape_v2" : "promscrape_v1";

			const int maxargs = 40;
			const char **argv = new const char *[maxargs];
			int i = 0;
			argv[i++] = promscrape.c_str();
			argv[i++] = address.c_str();
			string web_addr;	// Keep scope until execv since we're putting the c_str() in argv
			// Web interface is currently only supported in promscrape v2
			if (m_configuration.m_prom_conf.prom_sd() &&
				promscrape::c_promscrape_web_enable.get_value())
			{
				argv[i++] = "--web.enable";
				web_addr = "--web.listen-address=" + promscrape::c_promscrape_web_sock.get_value();
				argv[i++] = web_addr.c_str();
			}
			argv[i++] = "--log.format=json";
			argv[i++] = log_level.c_str();
			if (!prom_conf_arg.empty())
			{
				argv[i++] = prom_conf_arg.c_str();
			}

			char *labels = new char[c_promscrape_labels.get_value().length() + 1];
			// source labels are only supported in promscrape v2
			if (m_configuration.m_prom_conf.prom_sd())
			{
				// Copy promscrape labels into new C char array so we can use strtok()
				std::strcpy(labels, c_promscrape_labels.get_value().c_str());
				for (char *arg = std::strtok(labels, " \t"); (i < (maxargs - 1)) && arg;
					arg = std::strtok(NULL, " \t"))
				{
					argv[i++] = arg;
				}
			}
			argv[i++] = (char *)NULL;

			execv((m_configuration.c_root_dir.get_value() + "/bin/" + promscrape).c_str(), const_cast<char* const *>(argv));

			delete[] labels;
			delete[] argv;

			return (EXIT_FAILURE);
		});
	}
#endif

	monitor_process.set_cleanup_function([=]() {
		this->m_sdchecks_pipes.reset();
		this->m_jmx_pipes.reset();
		this->m_mounted_fs_reader_pipe.reset();
		this->m_statsite_pipes.reset();
		m_statsite_forwarder_pipe.reset();
		this->m_cointerface_pipes.reset();
#ifndef CYGWING_AGENT
		for (const auto& queue : {"/sdc_app_checks_in",
		                          "/sdc_app_checks_out",
		                          "/sdc_mounted_fs_reader_out",
		                          "/sdc_mounted_fs_reader_in",
		                          "/sdc_sdjagent_out",
		                          "/sdc_sdjagent_in",
		                          "/sdc_statsite_forwarder_in"})
		{
			posix_queue::remove(queue);
		}

		coclient::cleanup();
		default_cpu_cgroup.remove(c_cgroup_cleanup_timeout_ms.get_value());
		cointerface_cpu_cgroup.remove(c_cgroup_cleanup_timeout_ms.get_value());
#endif
	});

	return monitor_process.run();
#else   // _WIN32
	return sdagent_main();
#endif  // _WIN32
}

void dragent_app::setup_coredumps()
{
#ifndef _WIN32
	struct rlimit core_limits = {};
	if (m_configuration.m_enable_coredump)
	{
		core_limits.rlim_cur = RLIM_INFINITY;
		core_limits.rlim_max = RLIM_INFINITY;
	}
	else
	{
		core_limits.rlim_cur = 0;
		core_limits.rlim_max = 0;
	}
	errno = 0;
	if (setrlimit(RLIMIT_CORE, &core_limits) != 0)
	{
		LOG_WARNING("Cannot set coredump limits: %s", strerror(errno));
	}
	else
	{
		LOG_DEBUG("Successfully set coredump limits");
	}
#endif  // _WIN32
}

//
// Get basic info about the system and log it
//
void dragent_app::log_sysinfo()
{
	struct sysinfo info;
	auto error = sysinfo(&info);
	if (error == 0)
	{
		LOG_INFO("System uptime: " + NumberFormatter::format(info.uptime) + "s");
	}
	else
	{
		LOG_WARNING("Cannot get system uptime");
	}
	struct utsname osname;
	if (uname(&osname) == 0)
	{
		LOG_INFO(string("Kernel version: ") + osname.release);
	}
	else
	{
		LOG_WARNING("Cannot get kernel version");
	}
}


bool dragent_app::create_file(const std::string& dir, const std::string& file_name)
{
	Path p;
	p.parseDirectory(dir).setFileName(file_name);

	File f(p);

	return f.createFile();
}

bool dragent_app::remove_file_if_exists(const std::string& dir, const std::string& file_name)
{
	bool ret = false;
	Path p;
	p.parseDirectory(dir).setFileName(file_name);

	File f(p);
	if(f.exists())
	{
		ret = true;
		f.remove();
	}
	return ret;
}

int dragent_app::sdagent_main()
{
	Poco::ErrorHandler::set(&m_error_handler);

	initialize_logging();

	setup_coredumps();

	log_sysinfo();

	//
	// Load the configuration
	//
	m_configuration.refresh_machine_id();
	if(dragent_configuration::c_enable_aws_metadata.get_value()) {
		m_configuration.refresh_aws_metadata();
	}
	m_configuration.print_configuration();

	if (m_configuration.load_error())
	{
		LOG_ERROR("Unable to load configuration file");
		return exit_code::SHUT_DOWN;
	}

	m_protocol_handler.set_root_dir(m_configuration.c_root_dir.get_value());

	// Set the configured default compression method
	protobuf_compressor_factory::set_default(protocol_handler::c_compression_enabled.get_value()
	                                             ? protocol_compression_method::GZIP
	                                             : protocol_compression_method::NONE);

	//
	// Set up bidirectional communication with statsite
	//
	if (m_statsite_pipes)
	{
		LOG_DEBUG(
		    "statsite pipes size in=" + NumberFormatter::format(m_statsite_pipes->inpipe_size()) +
		    " out=" + NumberFormatter::format(m_statsite_pipes->outpipe_size()));
		m_sinsp_worker.set_statsite_pipes(m_statsite_pipes);
	}

	//
	// Gather identifying information about this agent instance
	//
	if (m_configuration.m_customer_id.empty())
	{
		LOG_ERROR("customerid not specified");
		return exit_code::RESTART;
	}

	if (m_configuration.machine_id() == "00:00:00:00:00:00")
	{
		LOG_ERROR("Invalid machine_id detected");
		return exit_code::RESTART;
	}

	//
	// Set up the memory watchdog
	//
	if (m_configuration.m_watchdog_enabled)
	{
		check_for_clean_shutdown();

#ifndef CYGWING_AGENT
		if (m_configuration.m_watchdog_heap_profiling_interval_s > 0)
		{
			// Heap profiling needs TCMALLOC_SAMPLE_PARAMETER to be set to a non-zero value
			// XXX hacky way to ensure that TCMALLOC_SAMPLE_PARAMETER was set correctly
			int32_t sample_period = 0;
			void** unused_ret = MallocExtension::instance()->ReadStackTraces(&sample_period);
			delete[] unused_ret;

			// If the env var isn't set, disable the dumping interval because it'll be garbage data
			if (sample_period <= 0)
			{
				LOG_ERROR(
				    "Disabling watchdog:heap_profiling_interval_s because "
				    "TCMALLOC_SAMPLE_PARAMETER is not set");
				m_configuration.m_watchdog_heap_profiling_interval_s = 0;
				ASSERT(false);
			}
		}
#endif
	}

	ExitCode exit_code;

	//
	// Start threads
	//
	if (!m_configuration.m_config_test)
	{
		m_pool.start(m_subprocesses_logger,
		             m_configuration.m_watchdog_subprocesses_logger_timeout_s);
	}

	//
	// Get the default compression values
	//
	// In the 10s flush world, compression is negotiated between the agent and
	// the collector. The configuration values determine 1. What values are
	// supported in the negotiation and 2. What values are illegal.
	//
	// These default values are what the agent will use going forward in the
	// legacy case and will be the basis of the negotiation in the protocol v5
	// case.
	//
	std::shared_ptr<protobuf_compressor> compressor =
	    protobuf_compressor_factory::get(protobuf_compressor_factory::get_default());

	////////////////
	// Here is where the top-level objects are created. These are the objects
	// that interact with the sysdig component and deliver flush data to the
	// connection_manager for delivery to the backend.
	////////////////

	connection_manager* cm = nullptr;
	sinsp::ptr inspector = nullptr;
	sinsp_analyzer* analyzer = nullptr;
	metric_serializer* serializer = nullptr;
	async_aggregator* aggregator = nullptr;
	try
	{
		cm = new connection_manager(
		    {
			    m_configuration.c_root_dir.get_value(),
			    m_configuration.m_server_addr,
			    m_configuration.m_server_port,
			    m_configuration.m_ssl_enabled,
			    m_configuration.m_ssl_ca_cert_paths,
			    m_configuration.m_ssl_ca_certificate,
			    m_configuration.m_promex_enabled,
			    m_configuration.m_promex_connect_url,
			    m_configuration.m_customer_id,
			    m_configuration.machine_id(),
			    m_configuration.c_root_dir.get_value()
		    },
		    &m_transmit_queue,
		    c_10s_flush_enabled.get_value()
		        ? std::initializer_list<dragent_protocol::protocol_version>{4, 5}
		        : std::initializer_list<dragent_protocol::protocol_version>{4},
		    {
		        {draiosproto::message_type::DUMP_REQUEST_START,
		         std::make_shared<dump_request_start_message_handler>(m_sinsp_worker)},
		        {draiosproto::message_type::DUMP_REQUEST_STOP,
		         std::make_shared<dump_request_stop_message_handler>(m_sinsp_worker)},
		        {draiosproto::message_type::CONFIG_DATA,
		         std::make_shared<config_data_message_handler>(m_configuration)},
		        {draiosproto::message_type::POLICIES,  // Legacy -- no longer used
		         std::make_shared<null_message_handler>()},
		        {draiosproto::message_type::POLICIES_V2,
		         std::make_shared<security_policies_v2_message_handler>(m_sinsp_worker)},
		        {draiosproto::message_type::COMP_CALENDAR,
		         std::make_shared<security_compliance_calendar_message_handler>(m_sinsp_worker)},
		        {draiosproto::message_type::COMP_RUN,
		         std::make_shared<security_compliance_run_message_handler>(m_sinsp_worker)},
		        {draiosproto::message_type::ORCHESTRATOR_EVENTS,
		         std::make_shared<security_orchestrator_events_message_handler>(m_sinsp_worker)},
		        {draiosproto::message_type::AGGREGATION_CONTEXT,
		         dragent::aggregator_limits::global_limits},
		        {draiosproto::message_type::BASELINES,  // Legacy -- no longer used
		         std::make_shared<null_message_handler>()}
		    });



		metric_limit_source::callback cb =
		    std::bind(&dragent_app::handle_metric_limit, 
		              this, 
		              std::placeholders::_1,
		              std::placeholders::_2);
		cm->register_metric_limit_destination(cb);

		m_pool.start(*cm, m_configuration.m_watchdog_connection_manager_timeout_s);

		// Must create inspector, then create analyzer, then setup inspector
		inspector = sinsp_factory::build();
		LOG_INFO("Created Sysdig inspector");

		metric_limits::sptr_t the_metric_limits =
		    metric_limits::build(m_configuration.m_metrics_filter,
		                         m_configuration.m_excess_metric_log,
		                         m_configuration.m_metrics_cache);

		label_limits::sptr_t the_label_limits =
		    label_limits::build(m_configuration.m_labels_filter,
		                        m_configuration.m_excess_labels_log,
		                        m_configuration.m_labels_cache);

		k8s_limits::sptr_t the_k8s_limits = k8s_limits::build(m_configuration.m_k8s_filter,
		                                                      m_configuration.m_excess_k8s_log,
		                                                      m_configuration.m_k8s_cache_size);

		std::shared_ptr<app_checks_proxy> the_app_checks_proxy = nullptr;
		if ((feature_manager::instance().get_enabled(APP_CHECKS) &&
		     !m_configuration.m_app_checks.empty()) ||
		    m_configuration.m_prom_conf.enabled())
		{
			bool app_check_thread = c_app_check_thread.get_value();
			the_app_checks_proxy =
			    std::make_shared<app_checks_proxy>(the_metric_limits, app_check_thread);
			if (app_check_thread)
			{
				m_pool.start(*the_app_checks_proxy.get(), c_app_check_timeout_s.get_value());
			}
		}

		std::shared_ptr<promscrape> the_promscrape = nullptr;
		if (m_configuration.m_prom_conf.enabled() && promscrape::c_use_promscrape.get_value())
		{
			auto interval_cb = [cm]() -> int {
				std::chrono::seconds s = cm->get_negotiated_aggregation_interval();
				return (s != std::chrono::seconds::max()) ? s.count() : 10;
			};
			the_promscrape = std::make_shared<promscrape>(the_metric_limits,
			                                              m_configuration.m_prom_conf,
			                                              c_promscrape_thread.get_value(),
			                                              interval_cb);
			if (c_promscrape_thread.get_value())
			{
				m_promscrape_proxy = std::make_shared<promscrape_proxy>(the_promscrape, &m_protocol_handler, cm);
				m_pool.start(*m_promscrape_proxy.get(), c_promscrape_timeout_s.get_value());
			}
		}

		analyzer = build_analyzer(inspector,
		                          m_aggregator_queue,
		                          the_metric_limits,
		                          the_label_limits,
		                          the_k8s_limits,
		                          the_app_checks_proxy,
		                          the_promscrape);
		LOG_INFO("Created analyzer");

		inspector->register_external_event_processor(*analyzer);
		init_inspector(inspector);
		LOG_INFO("Configured inspector");

		// There's an interesting dependency graph situation here. The
		// sinsp_worker is a member variable of the dragent_app class, and as
		// such gets created and initialized in the constructor. However, the
		// inspector and analyzer are created and initialized here, with much
		// pageantry. The init function exists to fully initialize the worker
		// after the creation of the other objects.
		m_sinsp_worker.init(inspector, analyzer);

		// Create and set up the aggregator
		aggregator = new async_aggregator(m_aggregator_queue,
		                                  m_serializer_queue,
		                                  300,
		                                  c_10s_flush_enabled.get_value() ? 10 : 0,
		                                  m_configuration.c_root_dir.get_value());
		aggregator->set_aggregation_interval_source(cm);

		m_pool.start(*aggregator, c_serializer_timeout_s.get_value());
		LOG_INFO("Created and started aggregator");

		if ((the_promscrape != nullptr) && promscrape::can_use_metrics_request_callback())
		{
			// Set metric request callback for async aggregator
			async_aggregator::metrics_request_cb metrics_request_cb =
				std::bind(&promscrape::metrics_request_callback, the_promscrape.get());

			aggregator->register_metrics_request_callback(metrics_request_cb);
			LOG_INFO("Registered promscrape metrics callback with aggregator");
		}

		// Create and set up the serializer
		auto s = new protobuf_metric_serializer(inspector,
		                                        m_configuration.c_root_dir.get_value(),
		                                        m_protocol_handler,
		                                        &m_serializer_queue,
		                                        &m_transmit_queue,
		                                        compressor,
		                                        cm);
		m_pool.start(*s, c_serializer_timeout_s.get_value());
		serializer = s;
		LOG_INFO("Created and started serializer");
	}
	catch (const sinsp_exception& e)
	{
		LOG_ERROR("Failed to setup internal components. Exception message: %s", e.what());
		running_state::instance().restart();
	}

	auto& state = running_state::instance();
	if (!state.is_terminated())
	{
		memdump_logger::register_callback(
		    std::make_shared<dragent_memdump_logger>(&m_capture_job_handler));

		//
		// Start the sinsp worker thread
		//
		m_capture_job_handler.init(m_sinsp_worker.get_inspector());
		m_pool.start(m_capture_job_handler, watchdog_runnable::NO_TIMEOUT);
		// sinsp_worker has not been changed to a watchdog_runnable
		ThreadPool::defaultPool().start(m_sinsp_worker, "sinsp_worker");

		// Start the container_size_requestor
		if (container_size_requestor_runnable::enabled())
		{
			sinsp_container_manager& mgr = m_sinsp_worker.get_container_manager();
			auto size_requestor = std::make_shared<container_size_requestor_runnable>(mgr);
			m_pool.start(size_requestor, 300 /*timeout*/);
		}

		enable_rest_server(m_configuration);

		// Setup the command line thread and message handler. If it isn't
		// enabled, create the message handler with a null thread so that
		// it can return the appropriate error.
		std::shared_ptr<command_line_runnable> cmdline;
		if (command_line_runnable::enabled())
		{
			cmdline = std::make_shared<command_line_runnable>(std::bind(&running_state::is_terminated, 
			                                                            &state));
			m_pool.start(cmdline, 300 /*timeout*/);
		}
		cm->set_message_handler(draiosproto::message_type::COMMAND_LINE_REQUEST,
		                        std::make_shared<command_line_request_message_handler>(cmdline, 
		                                                                               m_protocol_handler, 
		                                                                               m_configuration));
	}

	uint64_t uptime_s = 0;

	///////////////////////////////
	// Main exec loop
	// This is where the dragent thread sits while the other threads do the
	// actual work involved in making the agent work. It sits here checking
	// the watch dog and monitoring config files until someone decides it's
	// time to terminate.
	//////////////////////////////
	while (!state.is_terminated())
	{
		if (m_configuration.m_watchdog_enabled)
		{
			watchdog_check(uptime_s);
		}

#ifdef CYGWING_AGENT
		if (m_windows_service_parent)
		{
			if (!m_windows_helpers.is_parent_service_running())
			{
				LOG_INFO("Windows service stopped");
				state.shut_down();
				break;
			}
		}
#endif
		if ((m_configuration.m_monitor_files_freq_sec > 0) &&
		    (uptime_s % m_configuration.m_monitor_files_freq_sec == 0))
		{
			monitor_files(uptime_s);
		}

		setup_startup_probe(*cm);

		Thread::sleep(1000);
		++uptime_s;
	}

	//
	// Begin cleanup
	//
	disable_rest_server();

#ifndef CYGWING_AGENT
	if (m_configuration.m_watchdog_heap_profiling_interval_s > 0)
	{
		// Do a throttled dump in case we don't have anything recent
		dump_heap_profile(uptime_s, true);
	}
#endif

	if (!state.is_terminated())
	{
		state.shut_down();
	}

	exit_code = ExitCode(state.exit_code());

	//
	// Shut. Down. Everything.
	//
	// This will stop everything in the default pool
	m_pool.stop_all();
	if (serializer)
	{
		serializer->stop();
	}
	if (aggregator)
	{
		aggregator->stop();
	}

	if (m_configuration.m_watchdog_enabled)
	{
		mark_clean_shutdown();
	}

	LOG_INFO("Terminating");
	memdump_logger::register_callback(nullptr);
	return exit_code;
}

void dragent_app::init_inspector(sinsp::ptr inspector)
{
	inspector->set_debug_mode(true);
	inspector->set_internal_events_mode(true);
	inspector->set_hostname_and_port_resolution_mode(false);
	inspector->set_large_envs(m_configuration.m_large_envs);
	inspector->disable_automatic_threadtable_purging();
	sinsp_library_config::init_library_configs(*inspector);

	//
	// Plug the sinsp logger into our one
	//
	inspector->set_log_callback(common_logger::sinsp_logger_callback);
	g_logger.disable_timestamps();

	// our max priority is TRACE so we end up with confusing naming here
	auto min_priority =
	    std::max(m_configuration.m_min_console_priority, m_configuration.m_min_file_priority);

	inspector->set_min_log_severity(static_cast<sinsp_logger::severity>(min_priority));
}

sinsp_analyzer* dragent_app::build_analyzer(
    const sinsp::ptr& inspector,
    flush_queue& flush_queue,
    const metric_limits::sptr_t& the_metric_limits,
    const label_limits::sptr_t& the_label_limits,
    const k8s_limits::sptr_t& the_k8s_limits,
    std::shared_ptr<app_checks_proxy_interface> the_app_checks_proxy,
    std::shared_ptr<promscrape> promscrape)
{
	sinsp_analyzer* analyzer = new sinsp_analyzer(
	    inspector.get(),
	    m_configuration.c_root_dir.get_value(),
	    m_internal_metrics,
	    m_protocol_handler,
	    m_protocol_handler,
	    m_protocol_handler,
	    m_protocol_handler,
	    &flush_queue,
	    [this]() -> bool { return m_capture_job_handler.get_job_in_progress(); },
	    the_metric_limits,
	    the_label_limits,
	    the_k8s_limits,
	    std::move(the_app_checks_proxy),
	    promscrape);
	sinsp_configuration* sconfig = analyzer->get_configuration();

	sconfig->set_mounts_filter(m_configuration.m_mounts_filter);
	sconfig->set_mounts_limit_size(m_configuration.m_mounts_limit_size);

	if (m_configuration.java_present() && feature_manager::instance().get_enabled(JMX))
	{
		analyzer->enable_jmx(protocol_handler::c_print_protobuf.get_value(),
		                     m_configuration.m_jmx_sampling);
	}

	if (m_statsite_pipes)
	{
		const bool enable_statsite_forwarder = configuration_manager::instance()
		                                           .get_config<bool>("statsd.use_forwarder")
		                                           ->get_value() ||
		                                       (m_configuration.m_mode == dragent_mode_t::NODRIVER);

		analyzer->set_statsd_iofds(m_statsite_pipes->get_io_fds(), enable_statsite_forwarder);
	}

	//
	// The machine id is the MAC address of the first physical adapter
	//
	sconfig->set_machine_id(m_configuration.machine_id());

	sconfig->set_customer_id(m_configuration.m_customer_id);

//
// kubernetes
//
#ifndef CYGWING_AGENT

	sconfig->set_k8s_delegated_nodes(m_configuration.m_k8s_delegated_nodes);

	if (m_configuration.m_k8s_extensions.size())
	{
		sconfig->set_k8s_extensions(m_configuration.m_k8s_extensions);
	}
	if (m_configuration.m_use_new_k8s)
	{
		analyzer->set_use_new_k8s(m_configuration.m_use_new_k8s);
		analyzer->set_k8s_local_update_frequency(m_configuration.m_k8s_local_update_frequency);
		analyzer->set_k8s_cluster_update_frequency(m_configuration.m_k8s_cluster_update_frequency);
	}
	sconfig->set_k8s_cluster_name(m_configuration.m_k8s_cluster_name);

	//
	// mesos
	//
	sconfig->set_mesos_credentials(m_configuration.m_mesos_credentials);
	if (!m_configuration.m_mesos_state_uri.empty())
	{
		sconfig->set_mesos_state_uri(m_configuration.m_mesos_state_uri);
		sconfig->set_mesos_state_original_uri(m_configuration.m_mesos_state_uri);
	}
	sconfig->set_mesos_autodetect_enabled(m_configuration.m_mesos_autodetect);
	sconfig->set_mesos_follow_leader(m_configuration.m_mesos_follow_leader);
	sconfig->set_mesos_timeout_ms(m_configuration.m_mesos_timeout_ms);

	// marathon
	sconfig->set_marathon_credentials(m_configuration.m_marathon_credentials);
	if (!m_configuration.m_marathon_uris.empty())
	{
		sconfig->set_marathon_uris(m_configuration.m_marathon_uris);
	}
	sconfig->set_marathon_follow_leader(m_configuration.m_marathon_follow_leader);
	sconfig->set_dcos_enterprise_credentials(m_configuration.m_dcos_enterprise_credentials);

	if (m_configuration.m_marathon_skip_labels.size())
	{
		sconfig->set_marathon_skip_labels(m_configuration.m_marathon_skip_labels);
	}
#endif  // CYGWING_AGENT

	// curl
	sconfig->set_curl_debug(m_configuration.m_curl_debug);

	// user-configured events
	sconfig->set_k8s_event_filter(m_configuration.m_k8s_event_filter);
	sconfig->set_docker_event_filter(m_configuration.m_docker_event_filter);
	sconfig->set_containerd_event_filter(m_configuration.m_containerd_event_filter);

	// percentiles
	sconfig->set_percentiles(m_configuration.m_percentiles, m_configuration.m_group_pctl_conf);
	analyzer->set_percentiles();

	sconfig->set_container_filter(m_configuration.m_container_filter);

	sconfig->set_go_k8s_user_events(m_configuration.m_go_k8s_user_events);

	sconfig->set_log_dir(m_configuration.m_log_dir);

	if (m_configuration.m_tracepoint_hits_threshold > 0)
	{
		sconfig->set_tracepoint_hits_threshold(m_configuration.m_tracepoint_hits_threshold,
		                                       m_configuration.m_tracepoint_hits_ntimes);
	}

	if (m_configuration.m_cpu_usage_max_sr_threshold > 0)
	{
		sconfig->set_cpu_max_sr_threshold(m_configuration.m_cpu_usage_max_sr_threshold,
		                                  m_configuration.m_cpu_usage_max_sr_ntimes);
	}

	if (feature_manager::instance().get_enabled(BASELINER))
	{
		LOG_INFO("Setting secure profiling (baselining)");
		analyzer->enable_secure_profiling();
	}

	if (feature_manager::instance().get_enabled(COMMAND_LINE_CAPTURE) ||
	    feature_manager::instance().get_enabled(SECURE_AUDIT))
	{
		LOG_INFO("Setting command lines capture");
		sconfig->set_executed_commands_capture_enabled(true);
		sconfig->set_command_lines_capture_mode(m_configuration.m_command_lines_capture_mode);
		sconfig->set_command_lines_include_container_healthchecks(
		    m_configuration.m_command_lines_include_container_healthchecks);
		sconfig->set_command_lines_valid_ancestors(m_configuration.m_command_lines_valid_ancestors);
	}

	if (m_configuration.m_capture_dragent_events)
	{
		LOG_INFO("Setting capture dragent events");
		sconfig->set_capture_dragent_events(m_configuration.m_capture_dragent_events);
	}

	sconfig->set_version(AGENT_VERSION);
	if(dragent_configuration::c_enable_aws_metadata.get_value()) {
		sconfig->set_instance_id(m_configuration.m_aws_metadata.m_instance_id);
		sconfig->set_account_id(m_configuration.m_aws_metadata.m_account_id);
		sconfig->set_region(m_configuration.m_aws_metadata.m_region);
	}
	sconfig->set_protocols_truncation_size(m_configuration.m_protocols_truncation_size);
	analyzer->set_fs_usage_from_external_proc(m_configuration.m_system_supports_containers);

#ifndef CYGWING_AGENT
	if (feature_manager::instance().get_enabled(APP_CHECKS))
	{
		analyzer->set_app_checks(m_configuration.m_app_checks);
	}

	analyzer->set_prometheus_conf(m_configuration.m_prom_conf);
	if (m_configuration.m_config_test)
	{
		m_configuration.m_custom_container.set_config_test(true);
	}

	analyzer->set_custom_container_conf(move(m_configuration.m_custom_container));
#endif

	sconfig->set_procfs_scan_procs(m_configuration.m_procfs_scan_procs);

	//
	// Load the chisels
	//
	for (auto chinfo : m_configuration.m_chisel_details)
	{
		LOG_INFO("Loading chisel " + chinfo.m_name);
		analyzer->add_chisel(&chinfo);
	}

	analyzer->initialize_chisels();

	analyzer->set_track_environment(m_configuration.m_track_environment);
	analyzer->set_envs_per_flush(m_configuration.m_envs_per_flush);
	analyzer->set_max_env_size(m_configuration.m_max_env_size);
	analyzer->set_env_blacklist(std::move(m_configuration.m_env_blacklist));
	analyzer->set_env_hash_ttl(m_configuration.m_env_hash_ttl);
	analyzer->set_env_emit(m_configuration.m_env_metrics, m_configuration.m_env_audit_tap);

	if (m_configuration.m_audit_tap_enabled)
	{
		analyzer->enable_audit_tap();
	}

	if (feature_manager::instance().get_enabled(SECURE_AUDIT))
	{
		analyzer->enable_secure_audit();
	}

	if (feature_manager::instance().get_enabled(NETWORK_TOPOLOGY))
	{
		analyzer->enable_network_topology();
	}

	analyzer->set_remotefs_enabled(m_configuration.m_remotefs_enabled);

	analyzer->init_cpu_profiler();

	return analyzer;
}

bool dragent_app::timeout_expired(int64_t last_activity_age_ns,
                                  uint64_t timeout_s,
                                  const char* label,
                                  const char* tail)
{
	if (timeout_s == 0 || last_activity_age_ns <= static_cast<int64_t>(timeout_s) * 1000000000LL)
	{
		return false;
	}

	char line[128];
	snprintf(line,
	         sizeof(line),
	         "watchdog: Detected %s stall, last activity %" PRId64 " ns ago%s\n",
	         label,
	         last_activity_age_ns,
	         tail);
	crash_handler::log_crashdump_message(line);

	return true;
}

void dragent_app::watchdog_check(uint64_t uptime_s)
{
	bool to_kill = false;

	if (m_sinsp_worker.get_last_loop_ns() && m_sinsp_worker.is_stall_fatal())
	{
		const int64_t diff_ns =
		    sinsp_utils::get_current_time_ns() - m_sinsp_worker.get_last_loop_ns();

		if (diff_ns < 0)
		{
			static ratelimit r;
			r.run([&] {
				LOG_WARNING("watchdog: sinsp_worker last activity " +
				            NumberFormatter::format(-diff_ns) + " ns in the future");
			});
		}
		else
		{
#if _DEBUG
			LOG_DEBUG("watchdog: sinsp_worker last activity " + NumberFormatter::format(diff_ns) +
			          " ns ago");
#endif
		}

		if (timeout_expired(diff_ns,
		                    m_configuration.m_watchdog_sinsp_worker_debug_timeout_s,
		                    "sinsp_worker",
		                    ", enabling tracing"))
		{
			pthread_kill(m_sinsp_worker.get_pthread_id(), SIGSTKFLT);
		}

		if (timeout_expired(diff_ns,
		                    m_configuration.m_watchdog_sinsp_worker_timeout_s,
		                    "sinsp_worker",
		                    ", terminating process"))
		{
			pthread_kill(m_sinsp_worker.get_pthread_id(), SIGABRT);
			to_kill = true;
		}

		if ((uptime_s % m_configuration.m_watchdog_analyzer_tid_collision_check_interval_s) == 0 &&
		    m_sinsp_worker.m_analyzer->should_terminate())
		{
			char line[128];
			snprintf(line, sizeof(line), "watchdog: too many tid collisions\n");
			crash_handler::log_crashdump_message(line);

			if (m_sinsp_worker.get_last_loop_ns())
			{
				char buf[1024];
				m_sinsp_worker.get_analyzer()->generate_memory_report(buf, sizeof(buf));
				crash_handler::log_crashdump_message(buf);
			}

			to_kill = true;
		}
	}

	if (m_protocol_handler.get_last_loop_ns() != 0)
	{
		int64_t diff_ns =
		    sinsp_utils::get_current_time_ns() - m_protocol_handler.get_last_loop_ns();

		if (diff_ns < 0)
		{
			static ratelimit r;
			r.run([&] {
				LOG_WARNING("watchdog: sinsp_data_handler last activity " +
				            NumberFormatter::format(-diff_ns) + " ns in the future");
			});
		}
		else
		{
#if _DEBUG
			LOG_DEBUG("watchdog: sinsp_data_handler last activity " +
			          NumberFormatter::format(diff_ns) + " ns ago");
#endif
		}

		if (timeout_expired(diff_ns,
		                    m_configuration.m_watchdog_sinsp_data_handler_timeout_s,
		                    "sinsp_data_handler",
		                    ""))
		{
			to_kill = true;
		}
	}

	auto unhealthy = m_pool.unhealthy_list();
	if (!unhealthy.empty())
	{
		for (const watchdog_runnable_pool::unhealthy_runnable& current : unhealthy)
		{
			if (current.health == watchdog_runnable::health::TIMEOUT)
			{
				char line[128];
				snprintf(line,
				         sizeof(line),
				         "watchdog: Detected %s stall, last activity %" PRId64
				         " ms ago with timeout %" PRId64 "\n",
				         current.runnable.name().c_str(),
				         current.since_last_heartbeat_ms,
				         current.runnable.timeout_ms());
				crash_handler::log_crashdump_message(line);
				pthread_kill(current.runnable.pthread_id(), SIGABRT);
			}
			else
			{
				LOG_FATAL("Detected %s fatal error, last activity %" PRId64 " ms ago\n",
				          current.runnable.name().c_str(),
				          current.since_last_heartbeat_ms);
			}
		}

		to_kill = true;
	}

#ifndef CYGWING_AGENT
	if (feature_manager::instance().get_enabled(COINTERFACE))
	{
		if (!m_coclient)
		{
			// Actually allocate the coclient object
			m_coclient = make_unique<coclient>(m_configuration.c_root_dir.get_value());
		}

		// Ping every 5 seconds. If it's ever more than
		// watchdog_cointerface_timeout_s seconds from a pong,
		// declare it stuck and kill it.
		//
		// Note that we use the time from the ping as the
		// liveness time. So if cointerface somehow falls
		// behind by more than the timeout, it gets declared
		// stuck.

		m_cointerface_ping_interval.run(
		    [this]() {
			    coclient::response_cb_t callback = [this](bool successful,
			                                              google::protobuf::Message* response_msg) {
				    if (successful)
				    {
					    sdc_internal::pong* pong = (sdc_internal::pong*)response_msg;
					    m_subprocesses_state["cointerface"].reset(pong->pid(),
					                                              pong->memory_used(),
					                                              pong->token());

					    m_cointerface_ready = pong->ready();
				    }
			    };

			    m_coclient->ping(time(NULL), callback);
		    },
		    sinsp_utils::get_current_time_ns());

		// Try to read any responses
		m_coclient->process_queue();
	}
#endif  // CYGWING_AGENT

	// We now have started all the subprocesses, so pass them to internal_metrics
	update_subprocesses();

	// We only want this to happen once
	if (TIME_TO_UPDATE_PROCESS_PRIORITY == uptime_s)
	{
		update_subprocesses_priority();
	}

	uint64_t memory;
	if (dragent_configuration::get_memory_usage_mb(&memory))
	{
#if _DEBUG
		LOG_DEBUG("watchdog: memory usage " + NumberFormatter::format(memory) + " MiB");
#endif

#ifndef CYGWING_AGENT
		const bool heap_profiling = (m_configuration.m_watchdog_heap_profiling_interval_s > 0);
		bool dump_heap = false;
		bool throttle = true;

		// Once the worker is looping, we can dump the initial
		// memory state for diffing against later dumps
		if (heap_profiling && m_last_dump_s == 0 && m_sinsp_worker.get_last_loop_ns() != 0)
		{
			LOG_INFO("watchdog: heap profiling enabled, dumping initial memory state");
			dump_heap = true;
			throttle = false;
		}

		uint64_t watchdog_max = c_watchdog_max_memory_usage_mb.get_value();
		uint64_t watchdog_warn = c_watchdog_warn_memory_usage_mb.get_value();
		if (feature_manager::instance().get_enabled(MEMDUMP))
		{
			uint64_t configured_memdump_size =
			    configuration_manager::instance().get_config<uint64_t>("memdump.size")->get_value();

			if (!c_watchdog_max_memory_usage_mb.is_set_in_config())
			{
				watchdog_max += configured_memdump_size / 1024 / 1024;
			}
			if (!c_watchdog_warn_memory_usage_mb.is_set_in_config())
			{
				watchdog_warn += configured_memdump_size / 1024 / 1024;
			}
		}
		if (watchdog_warn > watchdog_max)
		{
			LOG_WARNING(
			    "watchdog:warn_memory_usage_mb cannot be higher than watchdog:max_memory_usage_mb. "
			    "Lowering Warn.");
			watchdog_warn = watchdog_max;
		}

		if (memory > watchdog_max)
		{
			char line[128];
			snprintf(line, sizeof(line), "watchdog: Fatal memory usage, %" PRId64 " MiB\n", memory);
			crash_handler::log_crashdump_message(line);

			if (m_sinsp_worker.get_last_loop_ns())
			{
				char buf[1024];
				m_sinsp_worker.get_analyzer()->generate_memory_report(buf, sizeof(buf));
				crash_handler::log_crashdump_message(buf);
			}

			if (heap_profiling)
			{
				dump_heap = true;
				throttle = false;
			}
			to_kill = true;
		}
		else if (memory > watchdog_warn)
		{
			LOG_NOTICE("watchdog: memory usage " + NumberFormatter::format(memory) + " MiB");
			if (heap_profiling)
			{
				dump_heap = true;
			}
		}

		if (dump_heap)
		{
			ASSERT(heap_profiling);
			dump_heap_profile(uptime_s, throttle);
		}
#endif
	}
	else
	{
		ASSERT(false);
	}

	if (to_kill)
	{
		log_watchdog_report();

		LOG_FATAL("Restarting dragent process immediately.");

		// Wait a bit to give time to the other threads to print stacktrace
		// or to terminate gracefully.
		sleep(5);
		char line[128];
		snprintf(line, sizeof(line), "watchdog: restarting immediately\n");
		crash_handler::log_crashdump_message(line);

		// Kill the process immediately. Monitor will restart it.
		//
		// The SIGKILL will immediately be handled by the operating system and
		// dragent will not get any more cpu time. After the operating system
		// handles the kill, the monitor will see that the dragent process died.
		// The monitor only has specific behavior for individual exit() codes, not
		// death by signals, so this gets handled in the default way, i.e. by
		// restarting the process.
		kill(getpid(), SIGKILL);
	}

	uint64_t now = sinsp_utils::get_current_time_ns() / ONE_SECOND_IN_NS;
	for (auto& proc : m_subprocesses_state)
	{
		auto& state = proc.second;
		if (state.valid())
		{
			LOG_DEBUG("valid subprocess: " + proc.first + ", " + to_string(state.memory_used()) +
			          " KiB");
			bool subprocess_to_kill = false;
			if (m_configuration.m_watchdog_max_memory_usage_subprocesses_mb.find(proc.first) !=
			        m_configuration.m_watchdog_max_memory_usage_subprocesses_mb.end() &&
			    state.memory_used() / 1024 >
			        m_configuration.m_watchdog_max_memory_usage_subprocesses_mb.at(proc.first))
			{
				LOG_CRITICAL("watchdog: " + proc.first + " using " +
					     to_string(state.memory_used() / 1024) + "MiB of memory, killing");
				subprocess_to_kill = true;
			}
			uint64_t last_loop_s = state.last_loop_s();
			uint64_t diff = 0;
			if (now > last_loop_s)
			{
				diff = now - last_loop_s;
			}
			else if (last_loop_s > now)
			{
				LOG_DEBUG("watchdog: " + proc.first + " last activity " +
				          NumberFormatter::format(last_loop_s - now) + " s in the future!");
			}
			if (m_configuration.m_watchdog_subprocesses_timeout_s.find(proc.first) !=
			        m_configuration.m_watchdog_subprocesses_timeout_s.end() &&
			    diff > m_configuration.m_watchdog_subprocesses_timeout_s.at(proc.first))
			{
				LOG_CRITICAL("watchdog: " + proc.first + " last activity " +
					     NumberFormatter::format(diff) + " s ago");
				// sdchecks implements the SIGHUP handler for handling stalls
				if (proc.first == "sdchecks")
				{
					kill(state.pid(), SIGHUP);
					state.reset();
				}
				else
				{
					subprocess_to_kill = true;
				}
			}
			if (subprocess_to_kill)
			{
				kill(state.pid(), SIGKILL);
				state.reset();
			}
		}
	}

	// Pass the (potentially) updated list of subprocesses to the internal metrics module.
	update_subprocesses();
}

void dragent_app::log_watchdog_report() const
{
	LOG_INFO("About to kill dragent. Listing all running processes...");
	m_pool.log_report();

	const uint64_t now_ns = sinsp_utils::get_current_time_ns();
	const int64_t sinsp_worker_diff_ns = now_ns - m_sinsp_worker.get_last_loop_ns();
	const int64_t data_handler_diff_ns = now_ns - m_protocol_handler.get_last_loop_ns();

	LOG_INFO("sinsp_worker last activity in  %" PRId64 " ms ago", sinsp_worker_diff_ns / 1000000);
	LOG_INFO("data_handler last activity in  %" PRId64 " ms ago", data_handler_diff_ns / 1000000);

	uint64_t now_s = now_ns / ONE_SECOND_IN_NS;
	for (auto& proc : m_subprocesses_state)
	{
		// Sdagent doesn't update this status (and we're currently running on it)
		// so don't bother printing it out.
		if (proc.first == "sdagent")
		{
			continue;
		}

		auto& state = proc.second;
		if (!state.valid())
		{
			continue;
		}

		const int64_t diff_s = now_s - state.last_loop_s();
		LOG_INFO("%s last activity %" PRId64 " s ago", proc.first.c_str(), diff_s);
	}
}

void dragent_app::update_subprocesses()
{
	internal_metrics::subprocs_t subprocs;

	for (auto& proc : m_subprocesses_state)
	{
		// The agent might not immediately know the pid for
		// each of the subprocesses, as it may not have read
		// the heartbeat message or gotten the ping
		// response. In that case, just skip the subprocess.

		if (proc.second.pid() > 0)
		{
			subprocs.insert(
			    std::pair<std::string, uint64_t>(proc.second.name(), proc.second.pid()));
		}
	}

	m_internal_metrics->set_subprocesses(subprocs);
}

void dragent_app::update_subprocesses_priority()
{
	for (const dragent_configuration::ProcessValueMap::value_type& value :
	     m_configuration.m_subprocesses_priority)
	{
		// This is the value configured by the yaml file. If it is the
		// default of 0, then we just ignore it.
		if (value.second == 0)
		{
			continue;
		}

		ProcessStateMap::const_iterator state = m_subprocesses_state.find(value.first);
		if (m_subprocesses_state.end() == state)
		{
			LOG_ERROR("Unable to change priority for process %s because pid was not saved",
			          value.first.c_str());
			continue;
		}

		LOG_INFO("Changing %s priority (%d) to %d",
		         value.first.c_str(),
		         state->second.pid(),
		         value.second);

		if (!process_helpers::change_priority(state->second.pid(), value.second))
		{
			LOG_ERROR("Unable to change priority for process %s", value.first.c_str());
		}
	}
}

#ifndef CYGWING_AGENT
void dragent_app::dump_heap_profile(uint64_t uptime_s, bool throttle)
{
	ASSERT(m_configuration.m_watchdog_heap_profiling_interval_s > 0);

	// Dump at most once every m_watchdog_heap_profiling_interval_s seconds
	// unless the caller tells us not to throttle
	if (throttle && (m_last_dump_s == 0 || (uptime_s - m_last_dump_s <
	                                        m_configuration.m_watchdog_heap_profiling_interval_s)))
	{
		return;
	}

	m_last_dump_s = uptime_s;

	// scripts/parse_heap_profiling.py depends on this format, so
	// don't change or add logs without updating the script
	auto malloc_extension = MallocExtension::instance();
	char heap_stats[2048];
	malloc_extension->GetStats(heap_stats, sizeof(heap_stats));
	static const auto separator = "\n---------------------\n";

	crash_handler::log_crashdump_message(heap_stats);

	string heap_sample;
	malloc_extension->GetHeapSample(&heap_sample);
	crash_handler::log_crashdump_message(separator);
	crash_handler::log_crashdump_message(heap_sample.c_str());
	crash_handler::log_crashdump_message(separator);
}
#endif

void dragent_app::check_for_clean_shutdown()
{
	if(m_had_unclean_shutdown)
	{
		LOG_DEBUG("Detected an unclean shutdown. Reporting to the backend");
		m_log_reporter.send_report(m_transmit_queue, sinsp_utils::get_current_time_ns());
	}
}

void dragent_app::mark_clean_shutdown()
{
	remove_file_if_exists(m_configuration.m_log_dir, K8S_PROBE_FILE);
}

Logger* dragent_app::make_console_channel(AutoPtr<Formatter> formatter)
{
	if (m_configuration.m_min_console_priority != -1)
	{
		AutoPtr<Channel> console_channel(new ConsoleChannel());
		AutoPtr<Channel> formatting_channel_console(
		    new FormattingChannel(formatter, console_channel));
		Logger& loggerc = Logger::create("DraiosLogC",
		                                 formatting_channel_console,
		                                 m_configuration.m_min_console_priority);
		return &loggerc;
	}
	return NULL;
}

Logger* dragent_app::make_event_channel()
{
	if (m_configuration.m_min_event_priority != -1)
	{
		AutoPtr<user_event_channel> event_channel = new user_event_channel();
		Logger& loggere =
		    Logger::create("DraiosLogE", event_channel, m_configuration.m_min_event_priority);
		m_sinsp_worker.set_user_event_queue(event_channel->get_event_queue());
		return &loggere;
	}
	return NULL;
}

void dragent_app::initialize_logging()
{
	//
	// Create the logs directory if it doesn't exist
	//
	File d(m_configuration.m_log_dir);
	d.createDirectories();
	Path p;
	p.parseDirectory(m_configuration.m_log_dir);
	p.setFileName("draios.log");
	string logsdir = p.toString();

	crash_handler::set_crashdump_file(p.toString());
	crash_handler::set_sinsp_worker(&m_sinsp_worker);

	//
	// Setup the logging
	//

	AutoPtr<globally_readable_file_channel> file_channel(
	    new globally_readable_file_channel(logsdir, m_configuration.m_globally_readable_log_files));

	file_channel->setProperty("purgeCount", std::to_string(m_configuration.m_log_rotate));
	file_channel->setProperty("rotation", std::to_string(m_configuration.m_max_log_size) + "M");
	file_channel->setProperty("archive", "timestamp");

	AutoPtr<Formatter> formatter(new PatternFormatter("%Y-%m-%d %H:%M:%S.%i, %P.%I, %p, %t"));
	AutoPtr<Channel> avoid_block(
	    new avoid_block_channel(file_channel, m_configuration.machine_id()));
	AutoPtr<Channel> formatting_channel_file(new FormattingChannel(formatter, avoid_block));

	// Create file logger at most permissive level (trace). This allows all messages to flow.
	// Log severity of messages actually emitted through the channel will be managed by
	// the consumers of the channel
	Logger& loggerf =
	    Logger::create("DraiosLogF", formatting_channel_file, Message::PRIO_TRACE);

	// Note: We are not responsible for managing the memory to which
	//       event_logger points; no free()/delete needed
	Logger* const event_logger = make_event_channel();
	if (event_logger != nullptr)
	{
		user_event_logger::register_callback(
		    std::make_shared<dragent_user_event_callback>(*event_logger,
		                                                  m_configuration.m_user_events_rate,
		                                                  m_configuration.m_user_max_burst_events));
	}

	g_log = unique_ptr<common_logger>(new common_logger(&loggerf,
							    m_configuration.m_min_file_priority,
							    c_log_file_component_overrides.get_value(),
							    make_console_channel(formatter)));

	g_log->set_observer(m_internal_metrics);

	// The following message was provided to Goldman Sachs (Oct 2018). Do not change.
	LOG_INFO("Agent starting (version " + string(AGENT_VERSION) + ")");
	common_logger_cache::log_and_purge();
}

void dragent_app::monitor_files(uint64_t uptime_s)
{
	static SHA1Engine engine;
	bool detected_change = false;

	// init the file states when called for the first time
	if (uptime_s == 0)
	{
		m_monitored_files.reserve(m_configuration.m_monitor_files.size());
		for (auto const& path : m_configuration.m_monitor_files)
		{
			std::string digest = "";
			struct stat f_stat;
			if (stat(path.c_str(), &f_stat) == 0)
			{
				digest = compute_sha1_digest(engine, path);
			}
			else
			{
				// if the file doesn't exist, still add an entry with
				// mtime of zero.
				f_stat.st_mtime = 0;
			}

			m_monitored_files.emplace_back(path, f_stat.st_mtime, digest);
		}
	}
	else
	{
		// iterate through files that need to be monitored and detect
		// changes to them by first checking for change in mtime and then
		// for changes in contents. if either has changed, update the
		// values in the state.
		for (auto& state : m_monitored_files)
		{
			struct stat f_stat;
			bool file_exists = stat(state.m_path.c_str(), &f_stat) == 0;
			if (file_exists && (f_stat.st_mtime != state.m_mod_time))
			{
				LOG_DEBUG("Modification time changed for file: " + state.m_path);
				state.m_mod_time = f_stat.st_mtime;

				// check modification of contents of the file
				auto new_digest = compute_sha1_digest(engine, state.m_path);
				if (new_digest != state.m_digest)
				{
					LOG_INFO("Detected changes to file: " + state.m_path);
					state.m_digest = new_digest;
					detected_change = true;
				}
			}
			else if (!file_exists && (state.m_mod_time != 0))
			{
				LOG_WARNING("Detected removal of file: " + state.m_path);
				detected_change = true;
			}
		}
	}

	// exit on detecting changes to files chosen to be monitored and
	// trigger restart of all related processes
	if (detected_change)
	{
		running_state::instance().restart_for_config_update();
	}
}

bool dragent_app::cointerface_ready() const
{
	bool ret = false;
	if(m_cointerface_ready)
	{
		static const std::chrono::time_point<std::chrono::steady_clock> start_wait = std::chrono::steady_clock::now();
		auto now = std::chrono::steady_clock::now();

		if(std::chrono::duration_cast<std::chrono::seconds>(now - start_wait).count() > c_wait_before_ready_sec->get_value())
		{
			ret = true;
		}
	}
	return ret;
}

void dragent_app::setup_startup_probe(const connection_manager& cm)
{
	if(!m_startup_probe_set)
	{
		if(cm.is_connected() && (c_wait_before_ready_sec->get_value() == 0 || cointerface_ready()))
		{
			m_startup_probe_set = create_file(m_configuration.m_log_dir, K8S_PROBE_FILE);
		}
	}
}

// This is running on the connection manager thread
void dragent_app::handle_metric_limit(bool has_limit, draiosproto::custom_metric_limit_value value)
{
	LOG_DEBUG("Metric limit callback");

	using nr = metric_forwarding_configuration::negotiation_result;
	metric_forwarding_configuration &mfc = metric_forwarding_configuration::instance();
	if (has_limit) 
	{
		if (draiosproto::custom_metric_limit_value::CUSTOM_METRIC_DEFAULT == value) 
		{
			mfc.set_negotiated_value(nr::USE_LEGACY_LIMITS);
		}
		else
		{
			mfc.set_negotiated_value(nr::USE_NEGOTIATED_VALUE, 
						 handshake_helpers::metric_limit_to_uint32(value));
		}
	}
	else
	{
		mfc.set_negotiated_value(nr::NEGOTIATION_NOT_SUPPORTED);
	}
}

const std::string dragent_app::K8S_PROBE_FILE = "running";
