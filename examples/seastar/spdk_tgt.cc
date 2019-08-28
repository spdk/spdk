#include <seastar/core/app-template.hh>
#include <seastar/core/future.hh>
#include <seastar/core/distributed.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/std-compat.hh>
#include <iostream>

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/env_dpdk.h"
#include "spdk/thread.h"
#include "spdk/rpc.h"
#include "spdk_internal/event.h"

struct seastar_lw_thread {
	struct spdk_thread *thread;
	seastar::promise<> p;
	seastar::compat::optional<seastar::reactor::poller> poller;

	seastar_lw_thread() : thread(NULL) {}

	seastar::future<> start() {
		std::string thread_name("thread");

		thread_name += std::to_string(seastar::engine().cpu_id());
		thread = spdk_thread_create(thread_name.c_str(), NULL);
		spdk_set_thread(thread);
		return seastar::make_ready_future();
	}

	bool poll_spdk_thread() {
		spdk_thread_poll(thread, 0, spdk_get_ticks());
		return true;
	}

	seastar::future<> run() {
		poller = seastar::reactor::poller::simple([&] { return poll_spdk_thread(); });
		return p.get_future();
	}

	seastar::future<> stop() {
		p.set_value();
		return seastar::make_ready_future();
	}

	seastar::future<> destroy() {
		spdk_thread_exit(thread);
		spdk_thread_destroy(thread);
		return seastar::make_ready_future();
	}
};

static int
seastar_schedule_thread(struct spdk_thread *thread)
{
	return 0;
}

static void
start_rpc(int rc, void *arg) {
	spdk_rpc_initialize("/var/tmp/spdk.sock");
	spdk_rpc_set_state(SPDK_RPC_RUNTIME);
}

seastar::distributed<seastar_lw_thread> g_lw_thread;

static void
subsystem_fini_done(void *arg) {
	g_lw_thread.invoke_on_all([] (auto &thread) {
		return thread.stop();
	}).get();
	g_lw_thread.invoke_on_all([] (auto &thread) {
		return thread.destroy();
	}).get();
}

seastar::future<> f() {
	struct spdk_env_opts opts;

	spdk_env_opts_init(&opts);
	if (spdk_env_init(&opts) == -EALREADY) {
		spdk_env_dpdk_post_init();
	}

	seastar::engine().at_exit([] {
		spdk_rpc_finish();
		spdk_subsystem_fini(subsystem_fini_done, NULL);
		return seastar::make_ready_future<>();
	});
	return seastar::async([&] {
		spdk_thread_lib_init(seastar_schedule_thread, 0);
		g_lw_thread.start().get0();
		g_lw_thread.invoke_on_all([] (auto &thread) {
			return thread.start();
		}).get();
		spdk_subsystem_init(start_rpc, NULL);
		g_lw_thread.invoke_on_all([] (auto &thread) {
			return thread.run();
		}).get();
		g_lw_thread.stop().get();
	});
}

int main(int argc, char** argv) {
	seastar::app_template app;
	try {
		app.run(argc, argv, f);
	} catch (...) {
		std::cerr << "Failed to start application: "
			  << std::current_exception() << "\n";
		return 1;
	}
	return 0;
}
