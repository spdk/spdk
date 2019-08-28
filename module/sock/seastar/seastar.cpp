#include <seastar/core/reactor.hh>
#include <seastar/core/deleter.hh>
#include <seastar/net/packet.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/ip.hh>
#include <iostream>

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/sock.h"
#include "spdk/util.h"
#include "spdk/endian.h"
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
		group_impl = NULL;
	}

	virtual ~spdk_seastar_sock() {}

	virtual void close() = 0;
};

class spdk_seastar_connected_sock : public spdk_seastar_sock {
public:
	seastar::connected_socket	sock;
	seastar::socket_address		local_address;
	seastar::socket_address		remote_address;
	seastar::input_stream<char>	input;
	seastar::output_stream<char>	output;
	std::list<seastar::temporary_buffer<char>> read_bufs;
	std::list<seastar::net::packet> write_packets;
	bool				write_in_progress;

	spdk_seastar_connected_sock(seastar::connected_socket& _sock,
				    seastar::socket_address _local_address,
				    seastar::socket_address _remote_address) :
		sock(std::move(_sock)),
		local_address(std::move(_local_address)),
		remote_address(std::move(_remote_address)),
		input(sock.input()), output(sock.output()),
		write_in_progress(false) {
		sock.set_nodelay(true);
	}

	~spdk_seastar_connected_sock() {}

	void close() {}
};

static void
handle_connection(spdk_seastar_connected_sock *sock) {
	(void)seastar::repeat([sock] {
		return sock->input.read().then([sock] (auto buf) {
			if (buf.size() > 0) {
				sock->read_bufs.push_back(std::move(buf));
				return seastar::make_ready_future<seastar::stop_iteration>(seastar::stop_iteration::no);
			} else {
				return sock->output.close().then([sock] {
					return seastar::make_ready_future<seastar::stop_iteration>(seastar::stop_iteration::yes);
				});
			}
		});
	}).then_wrapped([sock] (auto&& f) {
		if (sock->spdk_closed) {
			delete sock;
		} else {
			sock->seastar_closed = true;
		}
		try {
			f.get();
		} catch (std::exception& e) {
		}
	});
}

static void
write_next_packet(spdk_seastar_connected_sock *sock)
{
	if (sock->write_packets.empty() || sock->write_in_progress || sock->seastar_closed) {
		return;
	}

	seastar::net::packet p(std::move(sock->write_packets.front()));
	sock->write_packets.pop_front();
	sock->write_in_progress = true;
	(void)sock->output.write(std::move(p)).handle_exception([sock](auto e) {
		if (sock->spdk_closed) {
			delete sock;
		} else {
			sock->seastar_closed = true;
		}
	}).then([sock] {
		if (!sock->seastar_closed) {
			return sock->output.flush();
		} else {
			return seastar::make_ready_future<>();
		}
	}).handle_exception([sock](auto e) {
		if (sock->spdk_closed) {
			delete sock;
		} else {
			sock->seastar_closed = true;
		}
	}).then([sock] {
		sock->write_in_progress = false;
		write_next_packet(sock);
	});
}

class spdk_seastar_listen_sock : public spdk_seastar_sock {
	seastar::server_socket	listener;
	std::list<spdk_seastar_connected_sock *> socks;

public:
	spdk_seastar_listen_sock(uint32_t ip, uint16_t port) : listener(seastar::listen(seastar::make_ipv4_address({ip, port})))
	{
	}

	~spdk_seastar_listen_sock() {}

	struct spdk_sock *next_sock()
	{
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
			auto connected_sock = new spdk_seastar_connected_sock(ar.connection,
									      std::move(sock->listener.local_address()),
									      std::move(ar.remote_address));

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

	strncpy(saddr, inet_ntoa((in_addr)sock->local_address.addr()), slen);
	if (sport) {
		*sport = sock->local_address.port();
	}
	strncpy(caddr, inet_ntoa((in_addr)sock->remote_address.addr()), clen);
	if (cport) {
		*cport = sock->remote_address.port();
	}
	return 0;
}

static struct spdk_sock *
spdk_seastar_sock_listen(const char *_ip, int port)
{
	struct spdk_seastar_listen_sock *sock;
	uint32_t ip;

	inet_aton(_ip, (struct in_addr *)&ip);
	sock = new spdk_seastar_listen_sock(from_be32((void *)&ip), port);
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
	spdk_sock *next_sock;

	next_sock = sock->next_sock();
	if (next_sock == NULL) {
		errno = EAGAIN;
	}

	return next_sock;
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
	printf("%s not implemented yet\n", __func__);
	return 0;
}

struct spdk_malloc_deleter_impl : seastar::deleter::impl {
	void *buf;
	spdk_malloc_deleter_impl(void *buf) : impl(seastar::deleter()), buf(buf) {}
	virtual ~spdk_malloc_deleter_impl() override { spdk_free(buf); }
};

static ssize_t
spdk_seastar_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	spdk_seastar_connected_sock *sock = (spdk_seastar_connected_sock *)_sock;
	ssize_t bytes = 0;
	seastar::net::packet p;
	int i;

	for (i = 0; i < iovcnt; i++) {
		size_t len = iov[i].iov_len;
		uint64_t valid_len;
		char *buf;

		do {
			buf = (char *)spdk_malloc(len, 0, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
			valid_len = len;
			spdk_vtophys(buf, &valid_len);
			if (valid_len != len) {
				/* Kludge - if valid_len gets truncated, it means the buffer crosses a
				 * huge page boundary and isn't valid for DMA.  So we'll just leak the
				 * buffer here.
				 */
				SPDK_ERRLOG("%p only valid for %d bytes, not %d\n",
					    buf, (int)valid_len, (int)len);
			}
		} while (valid_len != len);

		memcpy((void *)buf, iov[i].iov_base, len);
		p.append(seastar::net::packet(seastar::net::fragment{buf, len},
					      seastar::deleter(new spdk_malloc_deleter_impl(buf))));
		bytes += len;
	}
	sock->write_packets.push_back(std::move(p));
	write_next_packet(sock);

	return bytes;
}

static int
spdk_seastar_sock_set_recvlowat(struct spdk_sock *_sock, int nbytes)
{
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
	int i = 0, j;

	for (spdk_seastar_connected_sock *sock : group->socks) {
		if (!sock->read_bufs.empty() || sock->seastar_closed) {
			socks[i++] = sock;
		}
		if (i == max_events) {
			break;
		}
	}

	for (j = 0; j < i; j++) {
		spdk_seastar_connected_sock *sock = (spdk_seastar_connected_sock *)socks[j];

		group->socks.remove(sock);
		group->socks.push_back(sock);
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
