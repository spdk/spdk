#include "spdk/stdinc.h"
#include "spdk/sock.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/net.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk_internal/sock_module.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <vcl/vppcom.h>

struct spdk_vcl_sock {
	struct spdk_sock base;
	uint32_t sh;
	bool connected;
	bool is_listener;
	bool is_ipv6;
	bool pending_connect;
	bool deferred_connect;
	bool registered;
	bool connect_registered;
	int connect_epfd;
	int worker_index;
	spdk_sock_connect_cb_fn connect_cb_fn;
	void *connect_cb_arg;
	char connect_addr[INET6_ADDRSTRLEN];
	int connect_port;
	char interface_name[IFNAMSIZ];
	TAILQ_ENTRY(spdk_vcl_sock) link;
};

struct spdk_vcl_sock_group_impl {
	struct spdk_sock_group_impl base;
	int epfd;
	int intrfd;
};

static struct spdk_sock_impl_opts g_vcl_impl_opts = {
	.recv_buf_size = DEFAULT_SO_RCVBUF_SIZE,
	.send_buf_size = DEFAULT_SO_SNDBUF_SIZE,
	.enable_recv_pipe = false,
	.enable_quickack = false,
	.enable_placement_id = PLACEMENT_NONE,
	.enable_zerocopy_send_server = false,
	.enable_zerocopy_send_client = false,
	.zerocopy_threshold = 0,
	.tls_version = 0,
	.enable_ktls = false,
	.psk_key = NULL,
	.psk_identity = NULL,
};

static pthread_once_t g_vcl_once = PTHREAD_ONCE_INIT;
static int g_vcl_init_rc;

#define __vcl_sock(sock) ((struct spdk_vcl_sock *)(sock))
#define __vcl_group(group) ((struct spdk_vcl_sock_group_impl *)(group))
#define VCL_INVALID_SH UINT32_MAX

static void
vcl_global_init_once(void)
{
	g_vcl_init_rc = vppcom_app_create("spdk_vcl");
}

static int
vcl_ensure_init(void)
{
	pthread_once(&g_vcl_once, vcl_global_init_once);
	if (g_vcl_init_rc != 0) {
		return g_vcl_init_rc;
	}

	if (vppcom_worker_index() < 0) {
		int rc = vppcom_worker_register();
		if (rc != 0 && rc != -EEXIST) {
			return rc;
		}
	}

	return 0;
}

static void
vcl_fill_endpoint(const char *ip, int port, vppcom_endpt_t *ep, uint8_t ipbuf[16], bool *is_ipv6)
{
	memset(ep, 0, sizeof(*ep));
	if (strchr(ip, ':') != NULL) {
		*is_ipv6 = true;
		ep->is_ip4 = 0;
		inet_pton(AF_INET6, ip, ipbuf);
	} else {
		*is_ipv6 = false;
		ep->is_ip4 = 1;
		inet_pton(AF_INET, ip, ipbuf);
	}
	ep->ip = ipbuf;
	ep->port = htons(port);
}

static int
vcl_sock_bind_worker(struct spdk_vcl_sock *sock)
{
	if (sock->worker_index < 0) {
		return vcl_ensure_init();
	}

	if (vppcom_worker_index() != sock->worker_index) {
		vppcom_worker_index_set(sock->worker_index);
	}

	return 0;
}

static int
vcl_sock_set_nonblock(uint32_t sh)
{
	int flags = O_RDWR | O_NONBLOCK;
	uint32_t len = sizeof(flags);

	return vppcom_session_attr(sh, VPPCOM_ATTR_SET_FLAGS, &flags, &len);
}

static inline int
vcl_sock_normalize_rc(ssize_t rc)
{
	if (rc == VPPCOM_EAGAIN || rc == VPPCOM_EWOULDBLOCK) {
		return -EAGAIN;
	}

	return (int)rc;
}

static int
vcl_sock_getaddr(struct spdk_sock *_sock, char *saddr, int slen, uint16_t *sport,
		 char *caddr, int clen, uint16_t *cport)
{
	struct spdk_vcl_sock *sock = __vcl_sock(_sock);
	vppcom_endpt_t ep = {};
	uint32_t len = sizeof(ep);
	char ipbuf[INET6_ADDRSTRLEN];
	uint8_t ep_ip[16];
	int rc;

	ep.ip = ep_ip;

	if (saddr) {
		rc = vppcom_session_attr(sock->sh, VPPCOM_ATTR_GET_LCL_ADDR, &ep, &len);
		if (rc == 0) {
			inet_ntop(ep.is_ip4 ? AF_INET : AF_INET6, ep.ip, ipbuf, sizeof(ipbuf));
			spdk_strcpy_pad(saddr, ipbuf, slen, '\0');
			if (sport) {
				*sport = ntohs(ep.port);
			}
		}
	}

	len = sizeof(ep);
	if (caddr) {
		ep.ip = ep_ip;
		rc = vppcom_session_attr(sock->sh, VPPCOM_ATTR_GET_PEER_ADDR, &ep, &len);
		if (rc == 0) {
			inet_ntop(ep.is_ip4 ? AF_INET : AF_INET6, ep.ip, ipbuf, sizeof(ipbuf));
			spdk_strcpy_pad(caddr, ipbuf, clen, '\0');
			if (cport) {
				*cport = ntohs(ep.port);
			}
		}
	}

	return 0;
}

static const char *
vcl_sock_get_interface_name(struct spdk_sock *_sock)
{
	struct spdk_vcl_sock *sock = __vcl_sock(_sock);

	if (sock->interface_name[0] == '\0') {
		spdk_strcpy_pad(sock->interface_name, "vcl", sizeof(sock->interface_name), '\0');
	}

	return sock->interface_name;
}

static int32_t
vcl_sock_get_numa_id(struct spdk_sock *_sock)
{
	return spdk_env_get_numa_id(spdk_env_get_current_core());
}

static struct spdk_vcl_sock *
vcl_sock_alloc(uint32_t sh)
{
	struct spdk_vcl_sock *sock = calloc(1, sizeof(*sock));

	if (sock == NULL) {
		return NULL;
	}

	TAILQ_INIT(&sock->base.queued_reqs);
	TAILQ_INIT(&sock->base.pending_reqs);
	sock->sh = sh;
	sock->connect_epfd = -1;
	sock->worker_index = sh == VCL_INVALID_SH ? -1 : vppcom_worker_index();
	return sock;
}

static ssize_t
vcl_sock_writev_internal(struct spdk_vcl_sock *sock, struct iovec *iov, int iovcnt)
{
	ssize_t total = 0;
	ssize_t rc;
	int i;

	for (i = 0; i < iovcnt; i++) {
		rc = vppcom_session_write(sock->sh, iov[i].iov_base, iov[i].iov_len);
		if (rc < 0) {
			return total > 0 ? total : rc;
		}
		total += rc;
		if (rc != (ssize_t)iov[i].iov_len) {
			break;
		}
	}

	return total;
}

static int
vcl_sock_complete_connect(struct spdk_vcl_sock *sock)
{
	sock->pending_connect = false;
	sock->connected = true;
	return 0;
}

static void
vcl_sock_connect_done(struct spdk_vcl_sock *sock, int status)
{
	spdk_sock_connect_cb_fn cb_fn = sock->connect_cb_fn;
	void *cb_arg = sock->connect_cb_arg;

	sock->connect_cb_fn = NULL;
	sock->connect_cb_arg = NULL;
	if (cb_fn) {
		cb_fn(cb_arg, status);
	}
}

static int
vcl_sock_start_connect(struct spdk_vcl_sock *sock)
{
	vppcom_endpt_t ep;
	uint8_t ipbuf[16];
	bool is_ipv6;
	int rc, sh;

	if (!sock->deferred_connect || sock->sh != VCL_INVALID_SH) {
		return 0;
	}

	rc = vcl_ensure_init();
	if (rc != 0) {
		return rc;
	}

	sh = vppcom_session_create(VPPCOM_PROTO_TCP, 1);
	if (sh < 0) {
		return sh;
	}

	rc = vcl_sock_set_nonblock(sh);
	if (rc != 0) {
		vppcom_session_close(sh);
		return rc;
	}

	vcl_fill_endpoint(sock->connect_addr, sock->connect_port, &ep, ipbuf, &is_ipv6);
	sock->sh = sh;
	sock->worker_index = vppcom_worker_index();
	sock->is_ipv6 = is_ipv6;
	sock->deferred_connect = false;

	rc = vppcom_session_connect(sh, &ep);
	if (rc == 0) {
		sock->connected = true;
		sock->pending_connect = false;
		vcl_sock_connect_done(sock, 0);
		return 0;
	}
	if (rc == VPPCOM_EINPROGRESS) {
		sock->pending_connect = true;
		return 0;
	}

	vppcom_session_close(sh);
	sock->sh = VCL_INVALID_SH;
	sock->worker_index = -1;
	sock->pending_connect = false;
	return rc;
}

static int
vcl_sock_connect_poller(struct spdk_vcl_sock *sock)
{
	struct epoll_event ev = {};
	int rc, n;

	if (sock->deferred_connect) {
		rc = vcl_sock_start_connect(sock);
		if (rc != 0 || !sock->pending_connect) {
			return rc;
		}
	}

	if (!sock->pending_connect) {
		return 0;
	}

	rc = vcl_sock_bind_worker(sock);
	if (rc != 0) {
		return rc;
	}

	if (sock->connect_epfd == -1) {
		sock->connect_epfd = vppcom_epoll_create();
		if (sock->connect_epfd < 0) {
			return sock->connect_epfd;
		}
	}

	if (!sock->connect_registered) {
		ev.events = EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
		ev.data.ptr = sock;
		rc = vppcom_epoll_ctl(sock->connect_epfd, EPOLL_CTL_ADD, sock->sh, &ev);
		if (rc != 0) {
			return rc;
		}
		sock->connect_registered = true;
	}

	n = vppcom_epoll_wait(sock->connect_epfd, &ev, 1, 0.001);
	if (n < 0) {
		return n;
	}
	if (n == 0) {
		return -EAGAIN;
	}
	if (ev.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
		vcl_sock_connect_done(sock, -ECONNRESET);
		return -ECONNRESET;
	}
	if (ev.events & EPOLLOUT) {
		rc = vcl_sock_complete_connect(sock);
		if (rc == 0) {
			vcl_sock_connect_done(sock, 0);
		} else {
			vcl_sock_connect_done(sock, rc);
		}
		return rc;
	}

	return -EAGAIN;
}

static int
vcl_sock_flush(struct spdk_sock *_sock)
{
	struct spdk_vcl_sock *sock = __vcl_sock(_sock);
	struct spdk_sock_request *req;
	struct iovec iovs[IOV_BATCH_SIZE];
	ssize_t rc;
	int iovcnt;
	uint64_t requested;

	rc = vcl_sock_connect_poller(sock);
	if (rc == -EAGAIN) {
		return -EAGAIN;
	}
	if (rc < 0) {
		return rc;
	}

	rc = vcl_sock_bind_worker(sock);
	if (rc != 0) {
		return rc;
	}

	while ((req = TAILQ_FIRST(&_sock->queued_reqs)) != NULL) {
		memset(iovs, 0, sizeof(iovs));
		requested = 0;
		iovcnt = spdk_sock_prep_req(req, iovs, 0, &requested);
		rc = vcl_sock_writev_internal(sock, iovs, iovcnt);
		if (rc < 0) {
			return vcl_sock_normalize_rc(rc);
		}
		req->internal.offset += rc;
		if ((uint64_t)rc < requested) {
			return -EAGAIN;
		}
		spdk_sock_request_pend(_sock, req);
		spdk_sock_request_put(_sock, req, 0);
	}

	return 0;
}

static struct spdk_sock *
vcl_sock_connect_internal(const char *ip, int port, struct spdk_sock_opts *opts,
			  bool async, spdk_sock_connect_cb_fn cb_fn, void *cb_arg)
{
	struct spdk_vcl_sock *sock;
	vppcom_endpt_t ep;
	uint8_t ipbuf[16];
	bool is_ipv6;
	int rc, sh;

	assert(async || (!cb_fn && !cb_arg));

	rc = vcl_ensure_init();
	if (rc != 0) {
		if (cb_fn) {
			cb_fn(cb_arg, rc);
		}
		return NULL;
	}

	sh = vppcom_session_create(VPPCOM_PROTO_TCP, async ? 1 : 0);
	if (sh < 0) {
		if (cb_fn) {
			cb_fn(cb_arg, sh);
		}
		return NULL;
	}

	sock = vcl_sock_alloc(sh);
	if (sock == NULL) {
		vppcom_session_close(sh);
		if (cb_fn) {
			cb_fn(cb_arg, -ENOMEM);
		}
		return NULL;
	}

	vcl_fill_endpoint(ip, port, &ep, ipbuf, &is_ipv6);
	sock->is_ipv6 = is_ipv6;
	if (async) {
		rc = vcl_sock_set_nonblock(sh);
		if (rc != 0) {
			free(sock);
			vppcom_session_close(sh);
			cb_fn(cb_arg, rc);
			return NULL;
		}
		sock->connect_cb_fn = cb_fn;
		sock->connect_cb_arg = cb_arg;
	}
	rc = vppcom_session_connect(sh, &ep);
	if (rc == 0) {
		if (!async) {
			rc = vcl_sock_set_nonblock(sh);
			if (rc != 0) {
				free(sock);
				vppcom_session_close(sh);
				return NULL;
			}
		}
		sock->connected = true;
		if (async) {
			vcl_sock_connect_done(sock, 0);
		}
	} else if (rc == VPPCOM_EINPROGRESS) {
		sock->pending_connect = true;
	} else {
		free(sock);
		vppcom_session_close(sh);
		if (cb_fn) {
			cb_fn(cb_arg, rc);
		}
		return NULL;
	}

	if (!async && sock->pending_connect) {
		free(sock);
		vppcom_session_close(sh);
		return NULL;
	}

	return &sock->base;
}

static struct spdk_sock *
vcl_sock_connect(const char *ip, int port, struct spdk_sock_opts *opts)
{
	return vcl_sock_connect_internal(ip, port, opts, false, NULL, NULL);
}

static struct spdk_sock *
vcl_sock_connect_async(const char *ip, int port, struct spdk_sock_opts *opts,
		       spdk_sock_connect_cb_fn cb_fn, void *cb_arg)
{
	struct spdk_vcl_sock *sock;

	if (vcl_ensure_init() != 0) {
		cb_fn(cb_arg, -ENODEV);
		return NULL;
	}

	sock = vcl_sock_alloc(VCL_INVALID_SH);
	if (sock == NULL) {
		cb_fn(cb_arg, -ENOMEM);
		return NULL;
	}

	spdk_strcpy_pad(sock->connect_addr, ip, sizeof(sock->connect_addr), '\0');
	sock->connect_port = port;
	sock->deferred_connect = true;
	sock->pending_connect = true;
	sock->connect_cb_fn = cb_fn;
	sock->connect_cb_arg = cb_arg;

	return &sock->base;
}

static struct spdk_sock *
vcl_sock_listen(const char *ip, int port, struct spdk_sock_opts *opts)
{
	struct spdk_vcl_sock *sock;
	vppcom_endpt_t ep;
	uint8_t ipbuf[16];
	bool is_ipv6;
	int rc, sh;

	rc = vcl_ensure_init();
	if (rc != 0) {
		return NULL;
	}

	sh = vppcom_session_create(VPPCOM_PROTO_TCP, 1);
	if (sh < 0) {
		return NULL;
	}

	vcl_fill_endpoint(ip, port, &ep, ipbuf, &is_ipv6);
	rc = vppcom_session_bind(sh, &ep);
	if (rc < 0) {
		vppcom_session_close(sh);
		return NULL;
	}

	rc = vppcom_session_listen(sh, 512);
	if (rc < 0) {
		vppcom_session_close(sh);
		return NULL;
	}

	sock = vcl_sock_alloc(sh);
	if (sock == NULL) {
		vppcom_session_close(sh);
		return NULL;
	}

	sock->connected = true;
	sock->is_listener = true;
	sock->is_ipv6 = is_ipv6;
	return &sock->base;
}

static struct spdk_sock *
vcl_sock_accept(struct spdk_sock *_sock)
{
	struct spdk_vcl_sock *listen_sock = __vcl_sock(_sock);
	struct spdk_vcl_sock *sock;
	vppcom_endpt_t ep = {};
	uint8_t ep_ip[16];
	int rc, sh;

	rc = vcl_sock_bind_worker(listen_sock);
	if (rc != 0) {
		errno = -rc;
		return NULL;
	}

	ep.ip = ep_ip;
	sh = vppcom_session_accept(listen_sock->sh, &ep, O_NONBLOCK);
	if (sh < 0) {
		errno = sh == -ENOENT ? EAGAIN : -sh;
		return NULL;
	}

	sock = vcl_sock_alloc(sh);
	if (sock == NULL) {
		vppcom_session_close(sh);
		return NULL;
	}

	sock->connected = true;
	sock->is_ipv6 = !ep.is_ip4;
	return &sock->base;
}

static int
vcl_sock_close(struct spdk_sock *_sock)
{
	struct spdk_vcl_sock *sock = __vcl_sock(_sock);
	int rc, close_rc = 0;

	rc = vcl_sock_bind_worker(sock);
	if (rc != 0) {
		close_rc = rc;
	}

	if (sock->connect_registered && sock->connect_epfd >= 0 && close_rc == 0) {
		vppcom_epoll_ctl(sock->connect_epfd, EPOLL_CTL_DEL, sock->sh, NULL);
		sock->connect_registered = false;
	}
	if (sock->connect_epfd >= 0) {
		rc = vppcom_session_close(sock->connect_epfd);
		if (close_rc == 0 && rc != 0) {
			close_rc = rc;
		}
		sock->connect_epfd = -1;
	}

	if (sock->sh != VCL_INVALID_SH) {
		rc = vppcom_session_close(sock->sh);
		if (close_rc == 0 && rc != 0) {
			close_rc = rc;
		}
	}

	free(sock);
	return close_rc;
}

static ssize_t
vcl_sock_recv(struct spdk_sock *_sock, void *buf, size_t len)
{
	struct spdk_vcl_sock *sock = __vcl_sock(_sock);
	int rc;

	rc = vcl_sock_connect_poller(sock);
	if (rc == -EAGAIN) {
		return -EAGAIN;
	}
	if (rc < 0) {
		return rc;
	}

	rc = vcl_sock_bind_worker(sock);
	if (rc < 0) {
		return rc;
	}

	rc = vppcom_session_read(sock->sh, buf, len);
	if (rc < 0) {
		return vcl_sock_normalize_rc(rc);
	}

	return rc;
}

static ssize_t
vcl_sock_readv(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_vcl_sock *sock = __vcl_sock(_sock);
	ssize_t total = 0;
	ssize_t rc;
	int i;

	rc = vcl_sock_connect_poller(sock);
	if (rc == -EAGAIN) {
		return -EAGAIN;
	}
	if (rc < 0) {
		return rc;
	}

	rc = vcl_sock_bind_worker(sock);
	if (rc < 0) {
		return rc;
	}

	for (i = 0; i < iovcnt; i++) {
		rc = vppcom_session_read(sock->sh, iov[i].iov_base, iov[i].iov_len);
		if (rc < 0) {
			return total > 0 ? total : vcl_sock_normalize_rc(rc);
		}
		total += rc;
		if (rc != (ssize_t)iov[i].iov_len) {
			break;
		}
	}

	return total;
}

static ssize_t
vcl_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_vcl_sock *sock = __vcl_sock(_sock);
	int rc;

	rc = vcl_sock_flush(_sock);
	if (rc < 0 && rc != -EAGAIN) {
		return rc;
	}
	if (!TAILQ_EMPTY(&_sock->queued_reqs)) {
		return -EAGAIN;
	}

	rc = vcl_sock_writev_internal(sock, iov, iovcnt);
	if (rc < 0) {
		return vcl_sock_normalize_rc(rc);
	}

	return rc;
}

static int
vcl_sock_recv_next(struct spdk_sock *_sock, void **buf, void **ctx)
{
	return -ENOTSUP;
}

static void
vcl_sock_writev_async(struct spdk_sock *sock, struct spdk_sock_request *req)
{
	spdk_sock_request_queue(sock, req);
	if (sock->queued_iovcnt >= IOV_BATCH_SIZE) {
		int rc = vcl_sock_flush(sock);
		if (rc < 0 && rc != -EAGAIN) {
			spdk_sock_abort_requests(sock);
		}
	}
}

static int
vcl_sock_set_recvlowat(struct spdk_sock *_sock, int nbytes)
{
	return 0;
}

static int
vcl_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	return 0;
}

static int
vcl_sock_set_sendbuf(struct spdk_sock *_sock, int sz)
{
	return 0;
}

static bool
vcl_sock_is_ipv6(struct spdk_sock *_sock)
{
	return __vcl_sock(_sock)->is_ipv6;
}

static bool
vcl_sock_is_ipv4(struct spdk_sock *_sock)
{
	return !__vcl_sock(_sock)->is_ipv6;
}

static bool
vcl_sock_is_connected(struct spdk_sock *_sock)
{
	struct spdk_vcl_sock *sock = __vcl_sock(_sock);
	int rc;

	rc = vcl_sock_connect_poller(sock);
	if (rc < 0) {
		return false;
	}

	rc = vcl_sock_bind_worker(sock);
	if (rc < 0) {
		return false;
	}

	return sock->connected && !sock->pending_connect;
}

static struct spdk_sock_group_impl *
vcl_sock_group_impl_get_optimal(struct spdk_sock *_sock, struct spdk_sock_group_impl *hint)
{
	return hint;
}

static struct spdk_sock_group_impl *
vcl_sock_group_impl_create(void)
{
	struct spdk_vcl_sock_group_impl *group;
	int rc;

	rc = vcl_ensure_init();
	if (rc != 0) {
		return NULL;
	}

	group = calloc(1, sizeof(*group));
	if (group == NULL) {
		return NULL;
	}

	group->epfd = vppcom_epoll_create();
	if (group->epfd < 0) {
		free(group);
		return NULL;
	}

	group->intrfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (group->intrfd < 0) {
		vppcom_session_close(group->epfd);
		free(group);
		return NULL;
	}

	return &group->base;
}

static int
vcl_sock_group_impl_add_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_vcl_sock_group_impl *group = __vcl_group(_group);
	struct spdk_vcl_sock *sock = __vcl_sock(_sock);
	struct epoll_event ev = {};
	int rc;

	if (!sock->pending_connect) {
		ev.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
	} else {
		ev.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP | EPOLLOUT;
	}
	ev.data.ptr = sock;
	if (sock->deferred_connect) {
		rc = vcl_sock_start_connect(sock);
		if (rc != 0) {
			return rc;
		}
	}
	rc = vcl_sock_bind_worker(sock);
	if (rc != 0) {
		return rc;
	}
	rc = vppcom_epoll_ctl(group->epfd, EPOLL_CTL_ADD, sock->sh, &ev);
	if (rc == 0) {
		sock->registered = true;
	}
	return rc;
}

static int
vcl_sock_group_impl_remove_sock(struct spdk_sock_group_impl *_group, struct spdk_sock *_sock)
{
	struct spdk_vcl_sock_group_impl *group = __vcl_group(_group);
	struct spdk_vcl_sock *sock = __vcl_sock(_sock);
	int rc = 0;

	if (sock->registered) {
		rc = vcl_sock_bind_worker(sock);
		if (rc != 0) {
			return rc;
		}

		rc = vppcom_epoll_ctl(group->epfd, EPOLL_CTL_DEL, sock->sh, NULL);
		if (rc == 0) {
			sock->registered = false;
		}
	}

	spdk_sock_abort_requests(_sock);
	return rc;
}

static int
vcl_sock_group_impl_poll(struct spdk_sock_group_impl *_group, int max_events, struct spdk_sock **socks)
{
	struct spdk_vcl_sock_group_impl *group = __vcl_group(_group);
	struct epoll_event events[MAX_EVENTS_PER_POLL];
	struct spdk_sock *sock;
	struct spdk_vcl_sock *vsock;
	int rc, count = 0, i;

	TAILQ_FOREACH(sock, &_group->socks, link) {
		vsock = __vcl_sock(sock);
		rc = vcl_sock_flush(sock);
		if (rc < 0 && rc != -EAGAIN) {
			spdk_sock_abort_requests(sock);
		}
	}

	rc = vppcom_epoll_wait(group->epfd, events, spdk_min(max_events, MAX_EVENTS_PER_POLL), 0.0);
	if (rc < 0) {
		return rc;
	}

	for (i = 0; i < rc && count < max_events; i++) {
		vsock = events[i].data.ptr;
		if (vsock == NULL) {
			continue;
		}
		if (vsock->pending_connect) {
			if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
				vcl_sock_connect_done(vsock, -ECONNRESET);
				continue;
			}
			if (!(events[i].events & EPOLLOUT)) {
				continue;
			}
			if (vcl_sock_complete_connect(vsock) != 0) {
				vcl_sock_connect_done(vsock, -ECONNRESET);
				continue;
			}
			vcl_sock_connect_done(vsock, 0);
		} else if (vsock->connect_cb_fn != NULL && vsock->connected) {
			vcl_sock_connect_done(vsock, 0);
		}
		if (vsock->base.cb_fn == NULL) {
			continue;
		}
		if (events[i].events & EPOLLIN) {
			socks[count++] = &vsock->base;
		}
	}

	return count;
}

static int
vcl_sock_group_impl_get_interruptfd(struct spdk_sock_group_impl *_group)
{
	return __vcl_group(_group)->intrfd;
}

static int
vcl_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	struct spdk_vcl_sock_group_impl *group = __vcl_group(_group);
	int rc = vppcom_session_close(group->epfd);

	close(group->intrfd);
	free(group);
	return rc;
}

static int
vcl_sock_impl_get_opts(struct spdk_sock_impl_opts *opts, size_t *len)
{
	if (!opts || !len) {
		return -EINVAL;
	}

	memset(opts, 0, *len);
	memcpy(opts, &g_vcl_impl_opts, spdk_min(*len, sizeof(g_vcl_impl_opts)));
	*len = spdk_min(*len, sizeof(g_vcl_impl_opts));
	return 0;
}

static int
vcl_sock_impl_set_opts(const struct spdk_sock_impl_opts *opts, size_t len)
{
	if (!opts) {
		return -EINVAL;
	}

	memcpy(&g_vcl_impl_opts, opts, spdk_min(len, sizeof(g_vcl_impl_opts)));
	return 0;
}

static int
vcl_net_impl_init(struct spdk_sock_initialize_opts *opts)
{
	return vcl_ensure_init();
}

static struct spdk_net_impl g_vcl_net_impl = {
	.name = "vcl",
	.init = vcl_net_impl_init,
	.getaddr = vcl_sock_getaddr,
	.get_interface_name = vcl_sock_get_interface_name,
	.get_numa_id = vcl_sock_get_numa_id,
	.connect = vcl_sock_connect,
	.connect_async = vcl_sock_connect_async,
	.listen = vcl_sock_listen,
	.accept = vcl_sock_accept,
	.close = vcl_sock_close,
	.recv = vcl_sock_recv,
	.readv = vcl_sock_readv,
	.writev = vcl_sock_writev,
	.recv_next = vcl_sock_recv_next,
	.writev_async = vcl_sock_writev_async,
	.readv_async = NULL,
	.flush = vcl_sock_flush,
	.set_recvlowat = vcl_sock_set_recvlowat,
	.set_recvbuf = vcl_sock_set_recvbuf,
	.set_sendbuf = vcl_sock_set_sendbuf,
	.is_ipv6 = vcl_sock_is_ipv6,
	.is_ipv4 = vcl_sock_is_ipv4,
	.is_connected = vcl_sock_is_connected,
	.group_impl_get_optimal = vcl_sock_group_impl_get_optimal,
	.group_impl_create = vcl_sock_group_impl_create,
	.group_impl_add_sock = vcl_sock_group_impl_add_sock,
	.group_impl_remove_sock = vcl_sock_group_impl_remove_sock,
	.group_impl_poll = vcl_sock_group_impl_poll,
	.group_impl_get_interruptfd = vcl_sock_group_impl_get_interruptfd,
	.group_impl_close = vcl_sock_group_impl_close,
	.get_opts = vcl_sock_impl_get_opts,
	.set_opts = vcl_sock_impl_set_opts,
};

SPDK_NET_IMPL_REGISTER(vcl, &g_vcl_net_impl);
