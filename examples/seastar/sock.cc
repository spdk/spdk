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
#include "spdk/sock.h"
#include "spdk/util.h"

static bool g_done;

struct seastar_lw_thread {
	struct spdk_thread *thread;

	seastar_lw_thread() : thread(NULL) {}

	seastar::future<> start() {
		std::string thread_name("thread");

		thread_name += std::to_string(seastar::engine().cpu_id());
		thread = spdk_thread_create(thread_name.c_str(), NULL);
		spdk_set_thread(thread);
		return seastar::make_ready_future();
	}

	seastar::future<> run() {
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
	if (g_sock != NULL) {
	//sock_cb(NULL, g_sock_group, g_sock);
	}
	return 1;
}

static void
start_rpc(int rc, void *arg) {
	spdk_rpc_initialize("/var/tmp/spdk.sock");
	spdk_rpc_set_state(SPDK_RPC_RUNTIME);
	g_listen_sock = spdk_sock_listen("127.0.0.1", 3260);
	g_sock_group = spdk_sock_group_create(NULL);
	g_accept_poller = spdk_poller_register(accept_poll, NULL, 1000);
	g_group_poller = spdk_poller_register(group_poll, NULL, 0);
}

static void
subsystem_fini_done(void *arg) {
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

seastar::future<> f() {
	struct spdk_env_opts opts;
	struct spdk_thread *thread;

	spdk_env_opts_init(&opts);
	spdk_env_init(&opts);

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

#include "spdk/log.h"
#include "spdk/sock.h"
#include "spdk_internal/sock.h"

class spdk_seastar_sock : public spdk_sock {

public:
	bool	spdk_closed;
	bool	seastar_closed;

	spdk_seastar_sock() : spdk_closed(false), seastar_closed(false) {
		cb_fn = NULL;
		cb_arg = NULL;
	}

	virtual ~spdk_seastar_sock() {}

	virtual void close() = 0;
};

class spdk_seastar_connected_sock : public spdk_seastar_sock {
public:
	seastar::connected_socket	sock;
	seastar::socket_address		remote_address;
	seastar::input_stream<char>	input;
	seastar::output_stream<char>	output;
	std::list<seastar::temporary_buffer<char>> read_bufs;

	spdk_seastar_connected_sock(seastar::connected_socket& _sock,
				    seastar::socket_address& _remote_address) :
		sock(std::move(_sock)), remote_address(std::move(_remote_address)),
		input(sock.input()), output(sock.output()) {
		std::cout << "accepted connection from " << remote_address << "\n";
	}

	~spdk_seastar_connected_sock() {}

	void close() {}
};

void handle_connection(spdk_seastar_connected_sock *sock) {
	(void)seastar::repeat([sock] {
		return sock->input.read().then([sock] (auto buf) {
			if (buf.size() > 0) {
				sock->read_bufs.push_back(std::move(buf));
				return seastar::make_ready_future<seastar::stop_iteration>(seastar::stop_iteration::no);
			} else {
				return sock->output.close().then([sock] {
					if (sock->spdk_closed) {
						delete sock;
					} else {
						sock->seastar_closed = true;
					}
					return seastar::make_ready_future<seastar::stop_iteration>(seastar::stop_iteration::yes);
				});
			}
		});
	}).then_wrapped([sock] (auto&& f) {
		try {
			f.get();
		} catch (std::exception& e) {
		}
	});
}

class spdk_seastar_listen_sock : public spdk_seastar_sock {
	seastar::server_socket	listener;
	std::list<spdk_seastar_connected_sock *> socks;

public:
	spdk_seastar_listen_sock() : listener(seastar::listen(seastar::make_ipv4_address({0x7F000001, 3260}))) {
	}

	~spdk_seastar_listen_sock() {}

	struct spdk_sock *next_sock() {
		spdk_seastar_connected_sock *new_sock;

		if (socks.empty()) {
			return NULL;
		}

		new_sock = socks.front();
		socks.pop_front();
		handle_connection(new_sock);

		return new_sock;
	}

	void close() {
		listener.abort_accept();
	}

	friend void listen(spdk_seastar_listen_sock *sock);
};

void listen(spdk_seastar_listen_sock *sock) {
	(void)seastar::keep_doing([sock] {

		return sock->listener.accept().then([sock] (seastar::accept_result ar) {
			auto connected_sock = new spdk_seastar_connected_sock(ar.connection, ar.remote_address);

			sock->socks.push_back(connected_sock);
		});
	}).then_wrapped([sock] (auto&& f) {
		try {
			f.get();
		} catch (std::exception& e) {
			delete sock;
		}
	});
}

static int
spdk_seastar_sock_getaddr(struct spdk_sock *_sock, char *saddr, int slen, uint16_t *sport,
			char *caddr, int clen, uint16_t *cport)
{
	spdk_seastar_connected_sock *sock = (spdk_seastar_connected_sock *)_sock;

	assert(sock != NULL);

	return 0;
}

static struct spdk_sock *
spdk_seastar_sock_listen(const char *ip, int port)
{
	struct spdk_seastar_listen_sock *sock;

	sock = new spdk_seastar_listen_sock;
	if (sock == NULL) {
		SPDK_ERRLOG("sock allocation failed\n");
		return NULL;
	}

	listen(sock);

	return sock;
}

static struct spdk_sock *
spdk_seastar_sock_connect(const char *ip, int port)
{
	printf("%s not implemented!\n", __func__);
	return NULL;
}

static struct spdk_sock *
spdk_seastar_sock_accept(struct spdk_sock *_sock)
{
	spdk_seastar_listen_sock *sock = (struct spdk_seastar_listen_sock *)_sock;
	spdk_seastar_connected_sock *connected_sock;

	return sock->next_sock();
}

static int
spdk_seastar_sock_close(struct spdk_sock *_sock)
{
	spdk_seastar_sock *sock = (spdk_seastar_sock *)_sock;

	sock->close();
	if (sock->seastar_closed) {
		delete sock;
	} else {
		sock->spdk_closed = true;
	}
	return 0;
}

static ssize_t
spdk_seastar_sock_recv(struct spdk_sock *_sock, void *_buf, size_t len)
{
	spdk_seastar_connected_sock *sock = (spdk_seastar_connected_sock *)_sock;
	ssize_t ret = 0;
	char *buf = (char *)_buf;

	while (len > 0) {
		if (sock->read_bufs.empty()) {
			break;
		}
		auto& sock_buf = sock->read_bufs.front();
		ssize_t size = spdk_min(sock_buf.size(), len);
		memcpy(buf, sock_buf.get(), size);
		buf += size;
		ret += size;
		len -= size;
		sock_buf.trim_front(size);
		if (sock_buf.size() == 0) {
			sock->read_bufs.pop_front();
		}
	}

	if (ret > 0) {
		return ret;
	} else if (sock->seastar_closed) {
		return 0;
	} else {
		errno = EAGAIN;
		return -1;
	}
}

static ssize_t
spdk_seastar_sock_readv(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	spdk_seastar_connected_sock *sock = (spdk_seastar_connected_sock *)_sock;

	return 0;
}

static ssize_t
spdk_seastar_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	spdk_seastar_connected_sock *sock = (spdk_seastar_connected_sock *)_sock;

	return 0;
}

static int
spdk_seastar_sock_set_recvlowat(struct spdk_sock *_sock, int nbytes)
{
	printf("%s not implemented yet\n", __func__);

	return 0;
}

static int
spdk_seastar_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	printf("%s not implemented yet\n", __func__);

	return 0;
}

static int
spdk_seastar_sock_set_sendbuf(struct spdk_sock *_sock, int sz)
{
	printf("%s not implemented yet\n", __func__);

	return 0;
}

static int
spdk_seastar_sock_set_priority(struct spdk_sock *_sock, int priority)
{
	return 0;
}

static bool
spdk_seastar_sock_is_ipv6(struct spdk_sock *_sock)
{
	return false;
}

static bool
spdk_seastar_sock_is_ipv4(struct spdk_sock *_sock)
{
	return true;
}

static int
spdk_seastar_sock_get_placement_id(struct spdk_sock *_sock, int *placement_id)
{
	return -1;
}

class spdk_seastar_sock_group_impl : public spdk_sock_group_impl {
public:
	std::list<spdk_seastar_connected_sock *> socks;

	spdk_seastar_sock_group_impl() : socks() {}

	~spdk_seastar_sock_group_impl() {}
};

static struct spdk_sock_group_impl *
spdk_seastar_sock_group_impl_create(void)
{
	spdk_seastar_sock_group_impl *group_impl;

	group_impl = new spdk_seastar_sock_group_impl;
	if (group_impl == NULL) {
		SPDK_ERRLOG("group_impl allocation failed\n");
		return NULL;
	}

	return group_impl;
}

static int
spdk_seastar_sock_group_impl_add_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	spdk_seastar_sock_group_impl *group = (spdk_seastar_sock_group_impl *)_group;
	spdk_seastar_connected_sock *sock = (spdk_seastar_connected_sock *)_sock;

	group->socks.push_back(sock);
	return 0;
}

static int
spdk_seastar_sock_group_impl_remove_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	spdk_seastar_sock_group_impl *group = (spdk_seastar_sock_group_impl *)_group;
	spdk_seastar_connected_sock *sock = (spdk_seastar_connected_sock *)_sock;

	group->socks.remove(sock);
	return 0;
}

static int
spdk_seastar_sock_group_impl_poll(struct spdk_sock_group_impl *_group, int max_events,
				struct spdk_sock **socks)
{
	spdk_seastar_sock_group_impl *group = (spdk_seastar_sock_group_impl *)_group;
	int i = 0;

	for (spdk_seastar_connected_sock *sock : group->socks) {
		if (!sock->read_bufs.empty() || sock->seastar_closed) {
			socks[i++] = sock;
		}
		if (i == max_events) {
			break;
		}
	}

	return i;
}

static int
spdk_seastar_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	spdk_seastar_sock_group_impl *group = (spdk_seastar_sock_group_impl *)_group;

	delete group;
	return 0;
}

static struct spdk_net_impl g_seastar_net_impl = {
	.name		= "seastar",
	.getaddr	= spdk_seastar_sock_getaddr,
	.connect	= spdk_seastar_sock_connect,
	.listen		= spdk_seastar_sock_listen,
	.accept		= spdk_seastar_sock_accept,
	.close		= spdk_seastar_sock_close,
	.recv		= spdk_seastar_sock_recv,
	.readv		= spdk_seastar_sock_readv,
	.writev		= spdk_seastar_sock_writev,
	.set_recvlowat	= spdk_seastar_sock_set_recvlowat,
	.set_recvbuf	= spdk_seastar_sock_set_recvbuf,
	.set_sendbuf	= spdk_seastar_sock_set_sendbuf,
	.set_priority	= spdk_seastar_sock_set_priority,
	.is_ipv6	= spdk_seastar_sock_is_ipv6,
	.is_ipv4	= spdk_seastar_sock_is_ipv4,
	.get_placement_id	= spdk_seastar_sock_get_placement_id,
	.group_impl_create	= spdk_seastar_sock_group_impl_create,
	.group_impl_add_sock	= spdk_seastar_sock_group_impl_add_sock,
	.group_impl_remove_sock = spdk_seastar_sock_group_impl_remove_sock,
	.group_impl_poll	= spdk_seastar_sock_group_impl_poll,
	.group_impl_close	= spdk_seastar_sock_group_impl_close,
};

SPDK_NET_IMPL_REGISTER(seastar, &g_seastar_net_impl);
