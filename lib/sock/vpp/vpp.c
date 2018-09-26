/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/log.h"
#include "spdk/sock.h"
#include "spdk/net.h"
#include "spdk/string.h"
#include "spdk_internal/sock.h"
#include "spdk/queue.h"

#include <svm/svm_fifo_segment.h>
#include <vlibmemory/api.h>
#include <vpp/api/vpe_msg_enum.h>
#include <vnet/session/application_interface.h>

#define vl_typedefs		/* define message structures */
#include <vpp/api/vpe_all_api_h.h>
#undef vl_typedefs

/* declare message handlers for each api */

#define vl_endianfun		/* define message structures */
#include <vpp/api/vpe_all_api_h.h>
#undef vl_endianfun

/* instantiate all the print functions we know about */
#define vl_print(handle, ...)
#define vl_printfun
#include <vpp/api/vpe_all_api_h.h>
#undef vl_printfun

/* VPP connection state */
enum spdk_vpp_state {
	VPP_STATE_START,
	VPP_STATE_ENABLED,
	VPP_STATE_ATTACHED,
	VPP_STATE_READY,
	VPP_STATE_DISCONNECTING,
	VPP_STATE_FAILED
};

#define MAX_TMPBUF 1024
#define PORTNUMLEN 32

/* VPP session state */
enum spdk_vpp_session_state {

	STATE_INIT,	/* Initial state */
	STATE_READY,	/* Ready for processing */
	STATE_DISCONNECT,

	STATE_FAILED
};

struct spdk_vpp_session {
	struct spdk_sock base;

	/* VPP app session */
	app_session_t app_session;

	uint32_t id;

	bool is_server;
	bool is_listen;

	uint64_t handle;
	uint32_t context;

	/* Listener fields */
	pthread_mutex_t accept_session_lock;
	uint32_t *accept_session_index_fifo;
};

static struct spdk_vpp_main {
	int my_client_index;
	enum spdk_vpp_state vpp_state;
	bool vpp_initialized;

	svm_queue_t *vl_input_queue;
	struct spdk_poller *vl_output_poller;
	svm_queue_t *vl_output_queue;
	struct spdk_poller *app_event_poller;
	svm_queue_t *app_event_queue;

	struct spdk_vpp_session *sessions;
	pthread_mutex_t session_get_lock;
	uword *session_index_by_vpp_handles;
	pthread_mutex_t session_lookup_lock;

} g_svm;

#define VPP_LISTENER_HANDLE(listener_handle) (listener_handle |= 1ULL << 63)

struct spdk_vpp_sock_group_impl {
	struct spdk_sock_group_impl base;
};

#define __vpp_session(sock) (struct spdk_vpp_session *)sock
#define __vpp_group_impl(group) (struct spdk_vpp_sock_group_impl *)group

static int
poll_vpp_session_api(void *arg)
{
	svm_queue_t *input_queue = arg;
	uword msg;

	assert(input_queue != NULL);
	if (input_queue->cursize) {
		if (svm_queue_sub(input_queue, (u8 *) &msg, SVM_Q_NOWAIT, 0) == 0) {
			vl_msg_api_handler((void *) msg);
		}
	}
	return 0;
}

static int
poll_vpp_session_event(void *arg)
{
	svm_queue_t *input_queue = arg;
	session_fifo_event_t _e, *e = &_e;

	assert(input_queue != NULL);
	if (input_queue->cursize) {
		if (svm_queue_sub(input_queue, (u8 *) e, SVM_Q_NOWAIT, 0) == 0) {
			if (e->event_type == FIFO_EVENT_APP_RX) {
				/* Handle RX event */
			}
		}
	}
	return 0;
}

/* This is required until sock.c API changes to asynchronous */
static int
_wait_for_session_state_change(struct spdk_vpp_session *session)
{
	time_t start = time(NULL);
	while (time(NULL) - start < 10) {
		if (session->app_session.session_state == STATE_FAILED) {
			errno = EADDRNOTAVAIL;
			return -1;
		}
		if (session->app_session.session_state == STATE_READY) {
			errno = 0;
			return 0;
		}
		poll_vpp_session_api(g_svm.vl_output_queue);
	}
	/* timeout */
	errno = ETIMEDOUT;
	return -1;
}

/******************************************************************************
 * Session management
 */
static struct spdk_vpp_session *
_spdk_vpp_session_create(void)
{
	struct spdk_vpp_session *session;

	pthread_mutex_lock(&g_svm.session_get_lock);

	pool_get(g_svm.sessions, session);
	memset(session, 0, sizeof(*session));
	pthread_mutex_init(&session->accept_session_lock, NULL);

	session->id = session - g_svm.sessions;
	session->app_session.session_state = STATE_INIT;

	pthread_mutex_unlock(&g_svm.session_get_lock);

	return session;
}

static struct spdk_vpp_session *
_spdk_vpp_session_get(uint32_t id)
{
	struct spdk_vpp_session *session;

	pthread_mutex_lock(&g_svm.session_get_lock);

	if (pool_is_free_index(g_svm.sessions, id)) {
		pthread_mutex_unlock(&g_svm.session_get_lock);
		return NULL;
	}
	session = pool_elt_at_index(g_svm.sessions, id);

	pthread_mutex_unlock(&g_svm.session_get_lock);

	return session;
}

static struct spdk_vpp_session *
_spdk_vpp_session_get_by_handle(uint64_t handle)
{
	struct spdk_vpp_session *session;
	uword *id;

	pthread_mutex_lock(&g_svm.session_lookup_lock);

	id = hash_get(g_svm.session_index_by_vpp_handles, handle);
	if (!id) {
		/* Could not find session by handle */
		pthread_mutex_unlock(&g_svm.session_lookup_lock);
		return NULL;
	}

	session = _spdk_vpp_session_get(*id);

	pthread_mutex_unlock(&g_svm.session_lookup_lock);

	return session;
}

static int
_spdk_vpp_session_set_handle(uint32_t id, uint64_t handle)
{

	pthread_mutex_lock(&g_svm.session_lookup_lock);

	hash_set(g_svm.session_index_by_vpp_handles, handle, id);

	pthread_mutex_unlock(&g_svm.session_lookup_lock);

	return 0;
}

static int
_spdk_vpp_session_free(struct spdk_vpp_session *session)
{
	/* Remove session */
	session = _spdk_vpp_session_get(session->id);
	if (session != NULL) {
		pthread_mutex_lock(&g_svm.session_lookup_lock);
		if (session->is_listen) {
			hash_unset(g_svm.session_index_by_vpp_handles,
				   VPP_LISTENER_HANDLE(session->handle));
		} else {
			hash_unset(g_svm.session_index_by_vpp_handles, session->handle);
		}
		pthread_mutex_unlock(&g_svm.session_lookup_lock);
		pthread_mutex_lock(&g_svm.session_get_lock);
		pool_put(g_svm.sessions, session);
		pthread_mutex_unlock(&g_svm.session_get_lock);
	} else {
		SPDK_ERRLOG("couldn't find session\n");
		return -EINVAL;
	}

	return 0;
}

static int
spdk_vpp_sock_getaddr(struct spdk_sock *_sock, char *saddr, int slen, char *caddr, int clen)
{
	struct spdk_vpp_session *session = __vpp_session(_sock);
	const char *result = NULL;

	assert(session != NULL);
	assert(g_svm.vpp_initialized);

	if (session->app_session.transport.is_ip4) {
		result = inet_ntop(AF_INET, &session->app_session.transport.lcl_ip.ip4.as_u8,
				   saddr, slen);
	} else {
		result = inet_ntop(AF_INET6, &session->app_session.transport.lcl_ip.ip6.as_u8,
				   saddr, slen);
	}
	if (result == NULL) {
		return -1;
	}

	if (session->app_session.transport.is_ip4) {
		result = inet_ntop(AF_INET, &session->app_session.transport.rmt_ip.ip4.as_u8,
				   caddr, clen);
	} else {
		result = inet_ntop(AF_INET6, &session->app_session.transport.rmt_ip.ip6.as_u8,
				   caddr, clen);
	}
	if (result == NULL) {
		return -1;
	}

	return 0;
}

enum spdk_vpp_create_type {
	SPDK_SOCK_CREATE_LISTEN,
	SPDK_SOCK_CREATE_CONNECT,
};

/******************************************************************************
 * Connect
 */
static void
vl_api_connect_session_reply_t_handler(vl_api_connect_session_reply_t *mp)
{
	struct spdk_vpp_session *session;
	svm_fifo_t *rx_fifo, *tx_fifo;

	if (mp->retval) {
		SPDK_ERRLOG("connection failed with code: %d\n", mp->retval);
		g_svm.vpp_state = VPP_STATE_FAILED;
		return;
	}

	session = _spdk_vpp_session_get(mp->context);
	if (session == NULL) {
		return;
	}

	session->app_session.vpp_evt_q = uword_to_pointer(mp->vpp_event_queue_address,
					 svm_queue_t *);

	rx_fifo = uword_to_pointer(mp->server_rx_fifo, svm_fifo_t *);
	rx_fifo->client_session_index = session->id;
	tx_fifo = uword_to_pointer(mp->server_tx_fifo, svm_fifo_t *);
	tx_fifo->client_session_index = session->id;

	session->app_session.rx_fifo = rx_fifo;
	session->app_session.tx_fifo = tx_fifo;

	/* Add handle to the lookup table */
	session->handle = mp->handle;
	_spdk_vpp_session_set_handle(session->id, session->handle);

	/* Set lcl addr */
	session->app_session.transport.is_ip4 = mp->is_ip4;
	memcpy(&session->app_session.transport.lcl_ip, mp->lcl_ip, sizeof(mp->lcl_ip));
	session->app_session.transport.lcl_port = mp->lcl_port;

	session->app_session.session_state = STATE_READY;
}

static int
_spdk_vpp_session_connect(struct spdk_vpp_session *session)
{
	vl_api_connect_sock_t *cmp;
	cmp = vl_msg_api_alloc(sizeof(*cmp));
	memset(cmp, 0, sizeof(*cmp));

	cmp->_vl_msg_id = ntohs(VL_API_CONNECT_SOCK);
	cmp->client_index = g_svm.my_client_index;
	cmp->context = session->id;

	cmp->vrf = 0 /* VPPCOM_VRF_DEFAULT */;
	cmp->is_ip4 = (session->app_session.transport.is_ip4);
	memcpy(cmp->ip, &session->app_session.transport.rmt_ip, sizeof(cmp->ip));
	cmp->port = session->app_session.transport.rmt_port;
	cmp->proto = TRANSPORT_PROTO_TCP;
	vl_msg_api_send_shmem(g_svm.vl_input_queue, (u8 *)&cmp);

	return _wait_for_session_state_change(session);
}

static void
vl_api_disconnect_session_reply_t_handler(vl_api_disconnect_session_reply_t *mp)
{
	struct spdk_vpp_session *session = 0;

	if (mp->retval) {
		SPDK_ERRLOG("Disconnecting session failed (%d).\n", mp->retval);
		return;
	}

	session = _spdk_vpp_session_get_by_handle(mp->handle);
	_spdk_vpp_session_free(session);
}

static void
vl_api_disconnect_session_t_handler(vl_api_disconnect_session_t *mp)
{
	struct spdk_vpp_session *session = 0;
	vl_api_disconnect_session_reply_t *rmp;
	int rv = 0;

	session = _spdk_vpp_session_get_by_handle(mp->handle);

	/* We need to postpone session deletion to inform upper layer */
	session->app_session.session_state = STATE_DISCONNECT;

	rmp = vl_msg_api_alloc(sizeof(*rmp));
	memset(rmp, 0, sizeof(*rmp));

	rmp->_vl_msg_id = ntohs(VL_API_DISCONNECT_SESSION_REPLY);
	rmp->retval = rv;
	rmp->handle = mp->handle;
	vl_msg_api_send_shmem(g_svm.vl_input_queue, (u8 *)&rmp);
}

static int
_spdk_vpp_session_disconnect(struct spdk_vpp_session *session)
{
	vl_api_disconnect_session_t *dmp;

	dmp = vl_msg_api_alloc(sizeof(*dmp));
	memset(dmp, 0, sizeof(*dmp));
	dmp->_vl_msg_id = ntohs(VL_API_DISCONNECT_SESSION);
	dmp->client_index = g_svm.my_client_index;
	dmp->handle = session->handle;
	vl_msg_api_send_shmem(g_svm.vl_input_queue, (u8 *)&dmp);

	return 0;
}

static void
vl_api_reset_session_t_handler(vl_api_reset_session_t *mp)
{
	vl_api_reset_session_reply_t *rmp;
	int rv = 0;

	/* TODO: reset session here by mp->handle and set rv if fail */

	rmp = vl_msg_api_alloc(sizeof(*rmp));
	memset(rmp, 0, sizeof(*rmp));
	rmp->_vl_msg_id = ntohs(VL_API_RESET_SESSION_REPLY);
	rmp->retval = rv;
	rmp->handle = mp->handle;
	vl_msg_api_send_shmem(g_svm.vl_input_queue, (u8 *)&rmp);
}

/******************************************************************************
 * Bind
 */
static void
vl_api_bind_sock_reply_t_handler(vl_api_bind_sock_reply_t *mp)
{
	struct spdk_vpp_session *session;

	/* Context should be set to the session index */
	session = _spdk_vpp_session_get(mp->context);

	if (mp->retval) {
		session->app_session.session_state = STATE_FAILED;
		return;
	}

	/* Set local address */
	session->app_session.transport.is_ip4 = mp->lcl_is_ip4;
	memcpy(&session->app_session.transport.lcl_ip, mp->lcl_ip, sizeof(mp->lcl_ip));
	session->app_session.transport.lcl_port = mp->lcl_port;

	/* Register listener */
	session->handle = mp->handle;
	_spdk_vpp_session_set_handle(session->id, VPP_LISTENER_HANDLE(mp->handle));

	/* Session binded, set listen state */
	session->is_listen = true;
	session->app_session.session_state = STATE_READY;
}

static void
vl_api_unbind_sock_reply_t_handler(vl_api_unbind_sock_reply_t *mp)
{
	struct spdk_vpp_session *session;

	if (mp->retval != 0) {
		SPDK_ERRLOG("Cannot unbind socket\n");
		return;
	}

	session = _spdk_vpp_session_get(mp->context);
	if (session == NULL) {
		SPDK_ERRLOG("Cannot find a session by context\n");
		return;
	}

	_spdk_vpp_session_free(session);
}

static void
_spdk_send_unbind_sock(struct spdk_vpp_session *session)
{
	vl_api_unbind_sock_t *ump;

	/* TODO: remove listener here and change state */

	ump = vl_msg_api_alloc(sizeof(*ump));
	memset(ump, 0, sizeof(*ump));

	ump->_vl_msg_id = ntohs(VL_API_UNBIND_SOCK);
	ump->client_index = g_svm.my_client_index;
	ump->handle = session->handle;
	ump->context = session->id;
	vl_msg_api_send_shmem(g_svm.vl_input_queue, (u8 *)&ump);
}

/******************************************************************************
 * Accept session
 */
static void
vl_api_accept_session_t_handler(vl_api_accept_session_t *mp)
{
	svm_fifo_t *rx_fifo, *tx_fifo;
	struct spdk_vpp_session *client_session, *listen_session;

	listen_session = _spdk_vpp_session_get_by_handle(
				 VPP_LISTENER_HANDLE(mp->listener_handle));
	if (!listen_session) {
		SPDK_ERRLOG("Listener not found\n");
		return;
	}

	/* Allocate local session for a client and set it up */
	client_session = _spdk_vpp_session_create();

	rx_fifo = uword_to_pointer(mp->server_rx_fifo, svm_fifo_t *);
	rx_fifo->client_session_index = client_session->id;
	tx_fifo = uword_to_pointer(mp->server_tx_fifo, svm_fifo_t *);
	tx_fifo->client_session_index = client_session->id;

	client_session->handle = mp->handle;
	client_session->context = mp->context;
	client_session->app_session.rx_fifo = rx_fifo;
	client_session->app_session.tx_fifo = tx_fifo;
	client_session->app_session.vpp_evt_q = uword_to_pointer(mp->vpp_event_queue_address,
						svm_queue_t *);

	client_session->is_server = true;
	client_session->app_session.transport.rmt_port = mp->port;
	client_session->app_session.transport.is_ip4 = mp->is_ip4;
	memcpy(&client_session->app_session.transport.rmt_ip, mp->ip, sizeof(mp->ip));

	/* Add it to lookup table */
	_spdk_vpp_session_set_handle(client_session->id, client_session->handle);
	client_session->app_session.transport.lcl_port = listen_session->app_session.transport.lcl_port;
	memcpy(&client_session->app_session.transport.lcl_ip, &listen_session->app_session.transport.lcl_ip,
	       sizeof(listen_session->app_session.transport.lcl_ip));
	client_session->app_session.transport.is_ip4 = listen_session->app_session.transport.is_ip4;

	client_session->app_session.session_state = STATE_READY;

	pthread_mutex_lock(&listen_session->accept_session_lock);

	clib_fifo_add1(listen_session->accept_session_index_fifo,
		       client_session->id);

	pthread_mutex_unlock(&listen_session->accept_session_lock);
}

static int
_spdk_vpp_session_listen(struct spdk_vpp_session *session)
{
	vl_api_bind_sock_t *bmp;

	if (session->is_listen) {
		/* Already in the listen state */
		return 0;
	}

	session->is_server = 1;
	bmp = vl_msg_api_alloc(sizeof(*bmp));
	memset(bmp, 0, sizeof(*bmp));

	bmp->_vl_msg_id = ntohs(VL_API_BIND_SOCK);
	bmp->client_index = g_svm.my_client_index;
	bmp->context = session->id;
	bmp->vrf = 0;
	bmp->is_ip4 = session->app_session.transport.is_ip4;
	memcpy(bmp->ip, &session->app_session.transport.lcl_ip, sizeof(bmp->ip));
	bmp->port = session->app_session.transport.lcl_port;
	bmp->proto = TRANSPORT_PROTO_TCP;

	vl_msg_api_send_shmem(g_svm.vl_input_queue, (u8 *)&bmp);

	return _wait_for_session_state_change(session);
}

static struct spdk_sock *
spdk_vpp_sock_create(const char *ip, int port, enum spdk_vpp_create_type type)
{
	struct spdk_vpp_session *session;
	int rc;
	uint8_t is_ip4 = 0;
	ip46_address_t addr_buf;

	if (ip == NULL) {
		return NULL;
	}

	session = _spdk_vpp_session_create();
	if (session == NULL) {
		SPDK_ERRLOG("_spdk_vpp_session_create() failed\n");
		errno = ENOMEM;
		return NULL;
	}

	/* Check address family */
	if (inet_pton(AF_INET, ip, &addr_buf.ip4.as_u8)) {
		is_ip4 = 1;
	} else if (inet_pton(AF_INET6, ip, &addr_buf.ip6.as_u8)) {
		is_ip4 = 0;
	} else {
		SPDK_ERRLOG("IP address with invalid format\n");
		errno = EAFNOSUPPORT;
		return NULL;
	}

	if (type == SPDK_SOCK_CREATE_LISTEN) {
		session->app_session.transport.is_ip4 = is_ip4;
		memcpy(&session->app_session.transport.lcl_ip, &addr_buf, sizeof(addr_buf));
		session->app_session.transport.lcl_port = htons(port);

		rc = _spdk_vpp_session_listen(session);
		if (rc != 0) {
			errno = -rc;
			SPDK_ERRLOG("session_listen() failed\n");
			pthread_mutex_lock(&g_svm.session_get_lock);
			pool_put(g_svm.sessions, session);
			pthread_mutex_unlock(&g_svm.session_get_lock);
			return NULL;
		}
	} else if (type == SPDK_SOCK_CREATE_CONNECT) {
		session->app_session.transport.is_ip4 = is_ip4;
		memcpy(&session->app_session.transport.rmt_ip, &addr_buf, sizeof(addr_buf));
		session->app_session.transport.rmt_port = htons(port);

		rc = _spdk_vpp_session_connect(session);
		if (rc != 0) {
			pthread_mutex_lock(&g_svm.session_get_lock);
			pool_put(g_svm.sessions, session);
			pthread_mutex_unlock(&g_svm.session_get_lock);
			return NULL;
		}
	}

	return &session->base;
}

static struct spdk_sock *
spdk_vpp_sock_listen(const char *ip, int port)
{
	if (!g_svm.vpp_initialized) {
		return NULL;
	}
	return spdk_vpp_sock_create(ip, port, SPDK_SOCK_CREATE_LISTEN);
}

static struct spdk_sock *
spdk_vpp_sock_connect(const char *ip, int port)
{
	if (!g_svm.vpp_initialized) {
		return NULL;
	}
	return spdk_vpp_sock_create(ip, port, SPDK_SOCK_CREATE_CONNECT);
}

static struct spdk_sock *
spdk_vpp_sock_accept(struct spdk_sock *_sock)
{
	struct spdk_vpp_session *listen_session = __vpp_session(_sock);
	struct spdk_vpp_session *client_session = NULL;
	u32 client_session_index = ~0;
	uword elts;
	int rv = 0;
	vl_api_accept_session_reply_t *rmp;

	assert(listen_session != NULL);
	assert(g_svm.vpp_initialized);

	if (listen_session->app_session.session_state != STATE_READY) {
		/* Listen session should be in the listen state */
		errno = EWOULDBLOCK;
		return NULL;
	}

	pthread_mutex_lock(&listen_session->accept_session_lock);

	elts = clib_fifo_elts(listen_session->accept_session_index_fifo);
	if (elts == 0) {
		/* No client sessions */
		errno = EAGAIN;
		pthread_mutex_unlock(&listen_session->accept_session_lock);
		return NULL;
	}

	clib_fifo_sub1(listen_session->accept_session_index_fifo,
		       client_session_index);

	pthread_mutex_unlock(&listen_session->accept_session_lock);

	client_session = _spdk_vpp_session_get(client_session_index);
	if (client_session == NULL) {
		SPDK_ERRLOG("client session closed or aborted\n");
		errno = ECONNABORTED;
		return NULL;
	}

	/*
	 * Send accept session reply
	 */
	rmp = vl_msg_api_alloc(sizeof(*rmp));
	memset(rmp, 0, sizeof(*rmp));
	rmp->_vl_msg_id = ntohs(VL_API_ACCEPT_SESSION_REPLY);
	rmp->retval = htonl(rv);
	rmp->context = client_session->context;
	rmp->handle = client_session->handle;
	vl_msg_api_send_shmem(g_svm.vl_input_queue, (u8 *)&rmp);

	return &client_session->base;
}

static int
spdk_vpp_sock_close(struct spdk_sock *_sock)
{
	struct spdk_vpp_session *session = __vpp_session(_sock);

	assert(session != NULL);
	assert(g_svm.vpp_initialized);

	session->app_session.session_state = STATE_DISCONNECT;

	if (session->is_listen) {
		_spdk_send_unbind_sock(session);
	} else {
		_spdk_vpp_session_disconnect(session);
	}

	return 0;
}

static ssize_t
spdk_vpp_sock_recv(struct spdk_sock *_sock, void *buf, size_t len)
{
	struct spdk_vpp_session *session = __vpp_session(_sock);
	int rc;
	svm_fifo_t *rx_fifo;

	assert(session != NULL);
	assert(g_svm.vpp_initialized);

	if (session->app_session.session_state == STATE_DISCONNECT) {
		/* Socket is disconnected */
		errno = 0;
		return 0;
	}

	rx_fifo = session->app_session.rx_fifo;

	ssize_t bytes = svm_fifo_max_dequeue(session->app_session.rx_fifo);
	if (bytes > len) {
		bytes = len;
	}
	if (bytes == 0) {
		errno = EAGAIN;
		return -1;
	}

	rc = svm_fifo_dequeue_nowait(rx_fifo, bytes, buf);
	if (rc < 0) {
		errno = -rc;
		return rc;
	}

	return rc;
}

static ssize_t
spdk_vpp_sock_writev(struct spdk_sock *_sock, struct iovec *iov, int iovcnt)
{
	struct spdk_vpp_session *session = __vpp_session(_sock);
	ssize_t total = 0;
	int i, rc;
	svm_fifo_t *tx_fifo;
	session_fifo_event_t evt;

	assert(session != NULL);
	assert(g_svm.vpp_initialized);

	tx_fifo = session->app_session.tx_fifo;

	for (i = 0; i < iovcnt; ++i) {
		rc = svm_fifo_enqueue_nowait(tx_fifo, iov[i].iov_len,
					     iov[i].iov_base);
		if (rc < 0) {
			if (total > 0) {
				break;
			} else {
				errno = -rc;
				return -1;
			}
		} else {
			total += rc;
		}
	}

	if (total > 0 && svm_fifo_set_event(tx_fifo)) {
		evt.fifo = tx_fifo;
		evt.event_type = FIFO_EVENT_APP_TX;
		svm_queue_add(session->app_session.vpp_evt_q, (u8 *)&evt, SVM_Q_WAIT);
	}

	return total;
}


/*
 * TODO: Check if there are similar parameters to configure in VPP
 * to three below.
 */
static int
spdk_vpp_sock_set_recvlowat(struct spdk_sock *_sock, int nbytes)
{
	assert(g_svm.vpp_initialized);

	return 0;
}

static int
spdk_vpp_sock_set_recvbuf(struct spdk_sock *_sock, int sz)
{
	assert(g_svm.vpp_initialized);

	return 0;
}

static int
spdk_vpp_sock_set_sendbuf(struct spdk_sock *_sock, int sz)
{
	assert(g_svm.vpp_initialized);

	return 0;
}

static bool
spdk_vpp_sock_is_ipv6(struct spdk_sock *_sock)
{
	struct spdk_vpp_session *session = __vpp_session(_sock);
	return !session->app_session.transport.is_ip4;
}

static bool
spdk_vpp_sock_is_ipv4(struct spdk_sock *_sock)
{
	struct spdk_vpp_session *session = __vpp_session(_sock);
	return session->app_session.transport.is_ip4;
}

static struct spdk_sock_group_impl *
spdk_vpp_sock_group_impl_create(void)
{
	struct spdk_vpp_sock_group_impl *group_impl;

	if (!g_svm.vpp_initialized) {
		return NULL;
	}

	group_impl = calloc(1, sizeof(*group_impl));
	if (group_impl == NULL) {
		SPDK_ERRLOG("sock_group allocation failed\n");
		errno = ENOMEM;
		return NULL;
	}

	return &group_impl->base;
}

static int
spdk_vpp_sock_group_impl_add_sock(struct spdk_sock_group_impl *_group,
				  struct spdk_sock *_sock)
{
	/* We expect that higher level do it for us */
	return 0;
}

static int
spdk_vpp_sock_group_impl_remove_sock(struct spdk_sock_group_impl *_group,
				     struct spdk_sock *_sock)
{
	/* We expect that higher level do it for us */
	return 0;
}

static bool
_spdk_vpp_session_read_ready(struct spdk_vpp_session *session)
{
	svm_fifo_t *rx_fifo = NULL;
	uint32_t ready = 0;

	if (session->app_session.session_state == STATE_DISCONNECT) {
		/* If session not found force reading to close it.
		 * NOTE: We're expecting here that upper layer will close
		 *       connection when next read fails.
		 */
		return true;
	}

	if (session->app_session.session_state == STATE_READY) {
		rx_fifo = session->app_session.rx_fifo;
		ready = svm_fifo_max_dequeue(rx_fifo);
	}

	return ready > 0;
}

static int
spdk_vpp_sock_group_impl_poll(struct spdk_sock_group_impl *_group, int max_events,
			      struct spdk_sock **socks)
{
	int num_events;
	struct spdk_sock *sock;
	struct spdk_vpp_session *session;

	assert(_group != NULL);
	assert(socks != NULL);
	assert(g_svm.vpp_initialized);

	num_events = 0;
	TAILQ_FOREACH(sock, &_group->socks, link) {
		session = __vpp_session(sock);
		if (_spdk_vpp_session_read_ready(session)) {
			if (num_events < max_events) {
				socks[num_events] = sock;
			}
			num_events++;
		}
	}

	return num_events;
}

static int
spdk_vpp_sock_group_impl_close(struct spdk_sock_group_impl *_group)
{
	return 0;
}

/******************************************************************************
 * Initialize and attach to the VPP
 */
static int
_spdk_vpp_app_attach(void)
{
	vl_api_application_attach_t *bmp;
	u32 fifo_size = 4 << 20;
	bmp = vl_msg_api_alloc(sizeof(*bmp));
	memset(bmp, 0, sizeof(*bmp));

	bmp->_vl_msg_id = ntohs(VL_API_APPLICATION_ATTACH);
	bmp->client_index = g_svm.my_client_index;
	bmp->context = ntohl(0xfeedface);
	bmp->options[APP_OPTIONS_FLAGS] = 0;
	bmp->options[APP_OPTIONS_PREALLOC_FIFO_PAIRS] = 16;
	bmp->options[APP_OPTIONS_RX_FIFO_SIZE] = fifo_size;
	bmp->options[APP_OPTIONS_TX_FIFO_SIZE] = fifo_size;
	bmp->options[APP_OPTIONS_ADD_SEGMENT_SIZE] = 128 << 20;
	bmp->options[APP_OPTIONS_SEGMENT_SIZE] = 256 << 20;
	vl_msg_api_send_shmem(g_svm.vl_input_queue, (u8 *)&bmp);

	return 0;
}
static void
vl_api_session_enable_disable_reply_t_handler(vl_api_session_enable_disable_reply_t *mp)
{
	if (mp->retval) {
		SPDK_ERRLOG("session enable/disable failed\n");
	} else {
		g_svm.vpp_state = VPP_STATE_ENABLED;
		_spdk_vpp_app_attach();
	}
}

static int
_spdk_vpp_session_enable_internal(u8 is_enable)
{
	vl_api_session_enable_disable_t *bmp;
	bmp = vl_msg_api_alloc(sizeof(*bmp));
	memset(bmp, 0, sizeof(*bmp));

	bmp->_vl_msg_id = ntohs(VL_API_SESSION_ENABLE_DISABLE);
	bmp->client_index = g_svm.my_client_index;
	bmp->context = htonl(0xfeedface);
	bmp->is_enable = is_enable;
	vl_msg_api_send_shmem(g_svm.vl_input_queue, (u8 *)&bmp);

	return 0;
}

static void
_spdk_vpp_session_enable(void *arg)
{
	_spdk_vpp_session_enable_internal(1);
}

static void
_spdk_vpp_application_attached(void *arg)
{
	g_svm.vpp_state = VPP_STATE_ATTACHED;
	spdk_net_framework_init_next(0);
}

static void
vl_api_application_attach_reply_t_handler(vl_api_application_attach_reply_t *mp)
{
	svm_fifo_segment_create_args_t a;
	int rv;

	if (mp->retval) {
		SPDK_ERRLOG("attach failed: %d\n", mp->retval);
		g_svm.vpp_state = VPP_STATE_FAILED;
		return;
	}

	if (mp->segment_name_length == 0) {
		SPDK_ERRLOG("segment_name_length zero\n");
		return;
	}

	memset(&a, 0, sizeof(a));
	a.segment_name = (char *)mp->segment_name;
	a.segment_size = mp->segment_size;

	assert(mp->app_event_queue_address);

	rv = svm_fifo_segment_attach(&a);
	if (rv) {
		SPDK_ERRLOG("svm_fifo_segment_attach ('%s') failed\n", mp->segment_name);
		return;
	}

	g_svm.app_event_queue = uword_to_pointer(mp->app_event_queue_address, svm_queue_t *);
	g_svm.app_event_poller = spdk_poller_register(poll_vpp_session_event, g_svm.app_event_queue, 100);

	spdk_thread_send_msg(spdk_get_thread(), _spdk_vpp_application_attached, NULL);
}

/* Detach */
static void
_spdk_vpp_application_detached(void *arg)
{
	g_svm.vpp_state = VPP_STATE_START;
	spdk_poller_unregister(&g_svm.vl_output_poller);
	vl_client_disconnect_from_vlib();
	spdk_net_framework_fini_next();
}

static void
vl_api_application_detach_reply_t_handler(vl_api_application_detach_reply_t *mp)
{
	if (mp->retval) {
		SPDK_ERRLOG("detach failed with error %d\n", mp->retval);
		g_svm.vpp_state = VPP_STATE_FAILED;
		return;
	}
	spdk_thread_send_msg(spdk_get_thread(), _spdk_vpp_application_detached, NULL);
}

static int
_spdk_vpp_app_detach(void)
{
	int rc;
	vl_api_application_detach_t *bmp;
	bmp = vl_msg_api_alloc(sizeof(*bmp));
	memset(bmp, 0, sizeof(*bmp));

	bmp->_vl_msg_id = ntohs(VL_API_APPLICATION_DETACH);
	bmp->client_index = g_svm.my_client_index;
	bmp->context = ntohl(0xfeedface);
	vl_msg_api_send_shmem(g_svm.vl_input_queue, (u8 *)&bmp);

	return 0;
}

static void
vl_api_map_another_segment_t_handler(vl_api_map_another_segment_t *mp)
{
	svm_fifo_segment_create_args_t a;
	int rv;

	memset(&a, 0, sizeof(a));
	a.segment_name = (char *) mp->segment_name;
	a.segment_size = mp->segment_size;
	rv = svm_fifo_segment_attach(&a);
	if (rv) {
		SPDK_ERRLOG("svm_fifo_segment_attach ('%s') failed\n", mp->segment_name);
		return;
	}
}

#define foreach_uri_msg                                 \
_(SESSION_ENABLE_DISABLE_REPLY, session_enable_disable_reply)   \
_(BIND_SOCK_REPLY, bind_sock_reply)                     \
_(UNBIND_SOCK_REPLY, unbind_sock_reply)                 \
_(ACCEPT_SESSION, accept_session)                       \
_(CONNECT_SESSION_REPLY, connect_session_reply)         \
_(DISCONNECT_SESSION, disconnect_session)               \
_(DISCONNECT_SESSION_REPLY, disconnect_session_reply)   \
_(RESET_SESSION, reset_session)                         \
_(APPLICATION_ATTACH_REPLY, application_attach_reply)   \
_(APPLICATION_DETACH_REPLY, application_detach_reply)	\
_(MAP_ANOTHER_SEGMENT, map_another_segment)		\

static int
spdk_vpp_net_framework_init(void)
{
	char *app_name;
	api_main_t *am = &api_main;

	clib_mem_init(0, 256 << 20);

	app_name = spdk_sprintf_alloc("SPDK_%d", getpid());
	if (app_name == NULL) {
		SPDK_ERRLOG("Cannot alloc memory for SPDK app name\n");
		return -ENOMEM;
	}

	/* Set up VPP handlers */
#define _(N,n)						\
	vl_msg_api_set_handlers(VL_API_##N, #n,		\
			vl_api_##n##_t_handler,		\
			vl_noop_handler,		\
			vl_api_##n##_t_endian,		\
			vl_api_##n##_t_print,		\
			sizeof(vl_api_##n##_t), 1);
	foreach_uri_msg;
#undef _

	if (vl_client_connect_to_vlib_no_rx_pthread("/vpe-api", app_name, 32 /* API_RX_Q_SIZE */) < 0) {
		return -1;
	}

	g_svm.vl_input_queue = am->shmem_hdr->vl_input_queue;
	g_svm.vl_output_queue = am->vl_input_queue;

	g_svm.my_client_index = am->my_client_index;
	g_svm.vpp_initialized = true;
	pthread_mutex_init(&g_svm.session_get_lock, NULL);
	pthread_mutex_init(&g_svm.session_lookup_lock, NULL);

	/* Preallocate sessions */
	pool_init_fixed(g_svm.sessions, 1024);

	g_svm.session_index_by_vpp_handles = hash_create(0, sizeof(uword));
	svm_fifo_segment_main_init(0x200000000ULL, 20);

	free(app_name);

	g_svm.vl_output_poller = spdk_poller_register(poll_vpp_session_api, g_svm.vl_output_queue, 1000);

	spdk_thread_send_msg(spdk_get_thread(), _spdk_vpp_session_enable, NULL);

	return 0;
}

/******************************************************************************
 * Register components
 */
static struct spdk_net_impl g_vpp_net_impl = {
	.name		= "vpp",
	.getaddr	= spdk_vpp_sock_getaddr,
	.connect	= spdk_vpp_sock_connect,
	.listen		= spdk_vpp_sock_listen,
	.accept		= spdk_vpp_sock_accept,
	.close		= spdk_vpp_sock_close,
	.recv		= spdk_vpp_sock_recv,
	.writev		= spdk_vpp_sock_writev,
	.set_recvlowat	= spdk_vpp_sock_set_recvlowat,
	.set_recvbuf	= spdk_vpp_sock_set_recvbuf,
	.set_sendbuf	= spdk_vpp_sock_set_sendbuf,
	.is_ipv6	= spdk_vpp_sock_is_ipv6,
	.is_ipv4	= spdk_vpp_sock_is_ipv4,
	.group_impl_create	= spdk_vpp_sock_group_impl_create,
	.group_impl_add_sock	= spdk_vpp_sock_group_impl_add_sock,
	.group_impl_remove_sock = spdk_vpp_sock_group_impl_remove_sock,
	.group_impl_poll	= spdk_vpp_sock_group_impl_poll,
	.group_impl_close	= spdk_vpp_sock_group_impl_close,
};

SPDK_NET_IMPL_REGISTER(vpp, &g_vpp_net_impl);

static void
spdk_vpp_net_framework_fini(void)
{
	if (g_svm.vpp_initialized) {
		spdk_poller_unregister(&g_svm.app_event_poller);
		_spdk_vpp_app_detach();
	}
}

static struct spdk_net_framework g_vpp_net_framework = {
	.name	= "vpp",
	.init	= spdk_vpp_net_framework_init,
	.fini	= spdk_vpp_net_framework_fini,
};

SPDK_NET_FRAMEWORK_REGISTER(vpp, &g_vpp_net_framework);
