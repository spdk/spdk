#include <seastar/core/app-template.hh>
#include <seastar/core/future.hh>
#include <seastar/core/distributed.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/deleter.hh>
#include <seastar/net/packet.hh>
#include <iostream>

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/env_dpdk.h"
#include "spdk/thread.h"
#include "spdk/rpc.h"
#include "spdk_internal/event.h"
#include "spdk/sock.h"
#include "spdk/util.h"
#include "spdk/endian.h"

static bool g_done;

struct seastar_lw_thread {
	struct spdk_thread *thread;

	seastar_lw_thread() : thread(NULL) {}

	seastar::future<> start()
	{
		std::string thread_name("thread");

		thread_name += std::to_string(seastar::engine().cpu_id());
		thread = spdk_thread_create(thread_name.c_str(), NULL);
		spdk_set_thread(thread);
		return seastar::make_ready_future();
	}

	seastar::future<> run()
	{
		return seastar::do_until([this] { return g_done; }, [this] {
			spdk_thread_poll(thread, 0, spdk_get_ticks());
			return seastar::make_ready_future<>();
		}).then([this] {
			spdk_thread_exit(thread);
			spdk_thread_destroy(thread);
		});
	}
};

static int
seastar_schedule_thread(struct spdk_thread *thread)
{
	return 0;
}

struct spdk_sock *g_sock;
struct spdk_sock *g_listen_sock;
struct spdk_sock_group *g_sock_group;
struct spdk_poller *g_accept_poller;
struct spdk_poller *g_group_poller;
struct spdk_poller *g_shutdown_poller;

ssize_t g_bytes_in = 0;
ssize_t g_bytes_out = 0;

static void
sock_cb(void *arg, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	ssize_t n;
	char buf[1024];
	struct iovec iov;

	n = spdk_sock_recv(sock, buf, sizeof(buf));

	if (n > 0) {
		g_bytes_in += n;
		iov.iov_base = buf;
		iov.iov_len = n;
		n = spdk_sock_writev(sock, &iov, 1);
		if (n > 0) {
			g_bytes_out += n;
		}
		return;
	} else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
		return;
	} else {
		/* Connection closed */
		spdk_sock_group_remove_sock(group, sock);
		spdk_sock_close(&sock);
		g_sock = NULL;
	}
}

static int
accept_poll(void *arg)
{
	struct spdk_sock *sock;

	sock = spdk_sock_accept(g_listen_sock);
	if (sock == NULL) {
		return 0;
	}

	g_sock = sock;
	spdk_sock_group_add_sock(g_sock_group, sock, sock_cb, NULL);
	return 1;
}

static int
group_poll(void *arg)
{
	spdk_sock_group_poll(g_sock_group);
	return 1;
}

static void
start_rpc(int rc, void *arg)
{
	spdk_rpc_initialize("/var/tmp/spdk.sock");
	spdk_rpc_set_state(SPDK_RPC_RUNTIME);
	g_listen_sock = spdk_sock_listen("192.168.0.2", 3260);
	g_sock_group = spdk_sock_group_create(NULL);
	g_accept_poller = spdk_poller_register(accept_poll, NULL, 1000);
	g_group_poller = spdk_poller_register(group_poll, NULL, 0);
}

static void
subsystem_fini_done(void *arg)
{
	g_done = true;
}

seastar::distributed<seastar_lw_thread> g_lw_thread;
seastar::promise<> g_shutdown_promise;

static int
try_shutdown(void *arg)
{
	int rc;

	rc = spdk_sock_group_close(&g_sock_group);
	if (rc == 0) {
		spdk_poller_unregister(&g_shutdown_poller);
		spdk_rpc_finish();
		spdk_subsystem_fini(subsystem_fini_done, NULL);
		g_shutdown_promise.set_value();
	}
	return 0;
}

seastar::future<> f()
{
	spdk_env_dpdk_post_init();

	seastar::engine().at_exit([] {
		spdk_poller_unregister(&g_accept_poller);
		spdk_poller_unregister(&g_group_poller);
		spdk_sock_close(&g_listen_sock);
		printf("bytes in =  %ju\n", g_bytes_in);
		printf("bytes out = %ju\n", g_bytes_out);
		g_shutdown_poller = spdk_poller_register(try_shutdown, NULL, 0);
		return g_shutdown_promise.get_future();
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

int main(int argc, char** argv)
{
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
