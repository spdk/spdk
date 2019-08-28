#include <seastar/core/app-template.hh>
#include <seastar/core/future.hh>
#include <seastar/core/distributed.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sleep.hh>
#include <iostream>

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/rpc.h"
#include "spdk_internal/event.h"

static struct seastar_lw_thread *g_lw_thread;

seastar::promise<> g_stopped;

struct seastar_lw_thread {
	struct spdk_thread *thread;
	bool done;

	seastar_lw_thread(struct spdk_thread *_thread) : done(false), thread(_thread) {
		seastar::do_until([this] { return done; }, [this] {
			spdk_thread_poll(thread, 0, spdk_get_ticks());
			return seastar::make_ready_future<>();
		}).then([this] {
			spdk_thread_exit(thread);
			spdk_thread_destroy(thread);
			g_stopped.set_value();
		}).get();
	}
};

static int
seastar_schedule_thread(struct spdk_thread *thread)
{
	struct seastar_lw_thread *lw_thread;

	lw_thread = new (spdk_thread_get_ctx(thread)) seastar_lw_thread(thread);
	if (g_lw_thread == NULL) {
		g_lw_thread = lw_thread;
	}

	return 0;
}

static void
start_rpc(int rc, void *arg) {
	spdk_rpc_initialize("/var/tmp/spdk.sock");
	spdk_rpc_set_state(SPDK_RPC_RUNTIME);
}

static void
subsystem_fini_done(void *arg) {
	g_lw_thread->done = true;
}

seastar::future<> f() {
	struct spdk_env_opts opts;
	struct spdk_thread *thread;

	spdk_env_opts_init(&opts);
	spdk_env_init(&opts);
	spdk_thread_lib_init(seastar_schedule_thread, sizeof(struct seastar_lw_thread));
	thread = spdk_thread_create("init_thread", NULL);
	spdk_set_thread(thread);
	spdk_subsystem_init(start_rpc, NULL);

	seastar::engine().at_exit([] {
		spdk_rpc_finish();
		spdk_subsystem_fini(subsystem_fini_done, NULL);
		return seastar::make_ready_future<>();
	});
	return g_stopped.get_future();
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
