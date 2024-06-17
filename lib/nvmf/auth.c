/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation
 */

#include "spdk/nvme.h"
#include "spdk/json.h"
#include "spdk/log.h"
#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"
#include "spdk_internal/nvme.h"

#include <openssl/rand.h>

#include "nvmf_internal.h"

#define NVMF_AUTH_DEFAULT_KATO_US (120ull * 1000 * 1000)
#define NVMF_AUTH_DIGEST_MAX_SIZE 64
#define NVMF_AUTH_DH_KEY_MAX_SIZE 1024

#define AUTH_ERRLOG(q, fmt, ...) \
	SPDK_ERRLOG("[%s:%s:%u] " fmt, (q)->ctrlr->subsys->subnqn, (q)->ctrlr->hostnqn, \
		    (q)->qid, ## __VA_ARGS__)
#define AUTH_DEBUGLOG(q, fmt, ...) \
	SPDK_DEBUGLOG(nvmf_auth, "[%s:%s:%u] " fmt, \
		      (q)->ctrlr->subsys->subnqn, (q)->ctrlr->hostnqn, (q)->qid, ## __VA_ARGS__)
#define AUTH_LOGDUMP(msg, buf, len) \
	SPDK_LOGDUMP(nvmf_auth, msg, buf, len)

enum nvmf_qpair_auth_state {
	NVMF_QPAIR_AUTH_NEGOTIATE,
	NVMF_QPAIR_AUTH_CHALLENGE,
	NVMF_QPAIR_AUTH_REPLY,
	NVMF_QPAIR_AUTH_SUCCESS1,
	NVMF_QPAIR_AUTH_SUCCESS2,
	NVMF_QPAIR_AUTH_FAILURE1,
	NVMF_QPAIR_AUTH_COMPLETED,
	NVMF_QPAIR_AUTH_ERROR,
};

struct spdk_nvmf_qpair_auth {
	enum nvmf_qpair_auth_state	state;
	struct spdk_poller		*poller;
	int				fail_reason;
	uint16_t			tid;
	int				digest;
	int				dhgroup;
	uint8_t				cval[NVMF_AUTH_DIGEST_MAX_SIZE];
	uint32_t			seqnum;
	struct spdk_nvme_dhchap_dhkey	*dhkey;
	bool				cvalid;
};

struct nvmf_auth_common_header {
	uint8_t		auth_type;
	uint8_t		auth_id;
	uint8_t		reserved0[2];
	uint16_t	t_id;
};

static void
nvmf_auth_request_complete(struct spdk_nvmf_request *req, int sct, int sc, int dnr)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	response->status.sct = sct;
	response->status.sc = sc;
	response->status.dnr = dnr;

	spdk_nvmf_request_complete(req);
}

static const char *
nvmf_auth_get_state_name(enum nvmf_qpair_auth_state state)
{
	static const char *state_names[] = {
		[NVMF_QPAIR_AUTH_NEGOTIATE] = "negotiate",
		[NVMF_QPAIR_AUTH_CHALLENGE] = "challenge",
		[NVMF_QPAIR_AUTH_REPLY] = "reply",
		[NVMF_QPAIR_AUTH_SUCCESS1] = "success1",
		[NVMF_QPAIR_AUTH_SUCCESS2] = "success2",
		[NVMF_QPAIR_AUTH_FAILURE1] = "failure1",
		[NVMF_QPAIR_AUTH_COMPLETED] = "completed",
		[NVMF_QPAIR_AUTH_ERROR] = "error",
	};

	return state_names[state];
}

static void
nvmf_auth_set_state(struct spdk_nvmf_qpair *qpair, enum nvmf_qpair_auth_state state)
{
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;

	if (auth->state == state) {
		return;
	}

	AUTH_DEBUGLOG(qpair, "auth state: %s\n", nvmf_auth_get_state_name(state));
	auth->state = state;
}

static void
nvmf_auth_disconnect_qpair(struct spdk_nvmf_qpair *qpair)
{
	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_ERROR);
	spdk_nvmf_qpair_disconnect(qpair);
}

static void
nvmf_auth_request_fail1(struct spdk_nvmf_request *req, int reason)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;

	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_FAILURE1);
	auth->fail_reason = reason;

	/* The command itself is completed successfully, but a subsequent AUTHENTICATION_RECV
	 * command will be completed with an AUTH_failure1 message
	 */
	nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS, 0);
}

static bool
nvmf_auth_digest_allowed(struct spdk_nvmf_qpair *qpair, uint8_t digest)
{
	struct spdk_nvmf_tgt *tgt = qpair->group->tgt;

	return tgt->dhchap_digests & SPDK_BIT(digest);
}

static bool
nvmf_auth_dhgroup_allowed(struct spdk_nvmf_qpair *qpair, uint8_t dhgroup)
{
	struct spdk_nvmf_tgt *tgt = qpair->group->tgt;

	return tgt->dhchap_dhgroups & SPDK_BIT(dhgroup);
}

static void
nvmf_auth_qpair_cleanup(struct spdk_nvmf_qpair_auth *auth)
{
	spdk_poller_unregister(&auth->poller);
	spdk_nvme_dhchap_dhkey_free(&auth->dhkey);
}

static int
nvmf_auth_timeout_poller(void *ctx)
{
	struct spdk_nvmf_qpair *qpair = ctx;
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;

	AUTH_ERRLOG(qpair, "authentication timed out\n");
	spdk_poller_unregister(&auth->poller);

	if (qpair->state == SPDK_NVMF_QPAIR_ENABLED) {
		/* Reauthentication timeout isn't considered to be a fatal failure */
		nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_COMPLETED);
		nvmf_auth_qpair_cleanup(auth);
	} else {
		nvmf_auth_disconnect_qpair(qpair);
	}

	return SPDK_POLLER_BUSY;
}

static int
nvmf_auth_rearm_poller(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	uint64_t timeout;

	timeout = ctrlr->feat.keep_alive_timer.bits.kato > 0 ?
		  ctrlr->feat.keep_alive_timer.bits.kato * 1000 :
		  NVMF_AUTH_DEFAULT_KATO_US;

	spdk_poller_unregister(&auth->poller);
	auth->poller = SPDK_POLLER_REGISTER(nvmf_auth_timeout_poller, qpair, timeout);
	if (auth->poller == NULL) {
		return -ENOMEM;
	}

	return 0;
}

static int
nvmf_auth_check_command(struct spdk_nvmf_request *req, uint8_t secp,
			uint8_t spsp0, uint8_t spsp1, uint32_t len)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;

	if (secp != SPDK_NVMF_AUTH_SECP_NVME) {
		AUTH_ERRLOG(qpair, "invalid secp=%u\n", secp);
		return -EINVAL;
	}
	if (spsp0 != 1 || spsp1 != 1) {
		AUTH_ERRLOG(qpair, "invalid spsp0=%u, spsp1=%u\n", spsp0, spsp1);
		return -EINVAL;
	}
	if (len != req->length) {
		AUTH_ERRLOG(qpair, "invalid length: %"PRIu32" != %"PRIu32"\n", len, req->length);
		return -EINVAL;
	}

	return 0;
}

static void *
nvmf_auth_get_message(struct spdk_nvmf_request *req, size_t size)
{
	if (req->length > 0 && req->iovcnt == 1 && req->iov[0].iov_len >= size) {
		return req->iov[0].iov_base;
	}

	return NULL;
}

static void
nvmf_auth_negotiate_exec(struct spdk_nvmf_request *req, struct spdk_nvmf_auth_negotiate *msg)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	struct spdk_nvmf_auth_descriptor *desc = NULL;
	/* These arrays are sorted from the strongest hash/dhgroup to the weakest, so the strongest
	 * hash/dhgroup pair supported by the host is always selected
	 */
	enum spdk_nvmf_dhchap_hash digests[] = {
		SPDK_NVMF_DHCHAP_HASH_SHA512,
		SPDK_NVMF_DHCHAP_HASH_SHA384,
		SPDK_NVMF_DHCHAP_HASH_SHA256
	};
	enum spdk_nvmf_dhchap_dhgroup dhgroups[] = {
		SPDK_NVMF_DHCHAP_DHGROUP_8192,
		SPDK_NVMF_DHCHAP_DHGROUP_6144,
		SPDK_NVMF_DHCHAP_DHGROUP_4096,
		SPDK_NVMF_DHCHAP_DHGROUP_3072,
		SPDK_NVMF_DHCHAP_DHGROUP_2048,
		SPDK_NVMF_DHCHAP_DHGROUP_NULL,
	};
	int digest = -1, dhgroup = -1;
	size_t i, j;

	if (auth->state != NVMF_QPAIR_AUTH_NEGOTIATE) {
		AUTH_ERRLOG(qpair, "invalid state: %s\n", nvmf_auth_get_state_name(auth->state));
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
		return;
	}

	auth->tid = msg->t_id;
	if (req->length < sizeof(*msg) || req->length != sizeof(*msg) + msg->napd * sizeof(*desc)) {
		AUTH_ERRLOG(qpair, "invalid message length: %"PRIu32"\n", req->length);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		return;
	}

	if (msg->sc_c != SPDK_NVMF_AUTH_SCC_DISABLED) {
		AUTH_ERRLOG(qpair, "scc mismatch\n");
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_SCC_MISMATCH);
		return;
	}

	for (i = 0; i < msg->napd; ++i) {
		if (msg->descriptors[i].auth_id == SPDK_NVMF_AUTH_TYPE_DHCHAP) {
			desc = &msg->descriptors[i];
			break;
		}
	}
	if (desc == NULL) {
		AUTH_ERRLOG(qpair, "no usable protocol found\n");
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_PROTOCOL_UNUSABLE);
		return;
	}
	if (desc->halen > SPDK_COUNTOF(desc->hash_id_list) ||
	    desc->dhlen > SPDK_COUNTOF(desc->dhg_id_list)) {
		AUTH_ERRLOG(qpair, "invalid halen=%u, dhlen=%u\n", desc->halen, desc->dhlen);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		return;
	}

	for (i = 0; i < SPDK_COUNTOF(digests); ++i) {
		if (!nvmf_auth_digest_allowed(qpair, digests[i])) {
			continue;
		}
		for (j = 0; j < desc->halen; ++j) {
			if (digests[i] == desc->hash_id_list[j]) {
				AUTH_DEBUGLOG(qpair, "selected digest: %s\n",
					      spdk_nvme_dhchap_get_digest_name(digests[i]));
				digest = digests[i];
				break;
			}
		}
		if (digest >= 0) {
			break;
		}
	}
	if (digest < 0) {
		AUTH_ERRLOG(qpair, "no usable digests found\n");
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_HASH_UNUSABLE);
		return;
	}

	for (i = 0; i < SPDK_COUNTOF(dhgroups); ++i) {
		if (!nvmf_auth_dhgroup_allowed(qpair, dhgroups[i])) {
			continue;
		}
		for (j = 0; j < desc->dhlen; ++j) {
			if (dhgroups[i] == desc->dhg_id_list[j]) {
				AUTH_DEBUGLOG(qpair, "selected dhgroup: %s\n",
					      spdk_nvme_dhchap_get_dhgroup_name(dhgroups[i]));
				dhgroup = dhgroups[i];
				break;
			}
		}
		if (dhgroup >= 0) {
			break;
		}
	}
	if (dhgroup < 0) {
		AUTH_ERRLOG(qpair, "no usable dhgroups found\n");
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_DHGROUP_UNUSABLE);
		return;
	}

	if (nvmf_auth_rearm_poller(qpair)) {
		nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, 1);
		nvmf_auth_disconnect_qpair(qpair);
		return;
	}

	auth->digest = digest;
	auth->dhgroup = dhgroup;
	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_CHALLENGE);
	nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS, 0);
}

static void
nvmf_auth_reply_exec(struct spdk_nvmf_request *req, struct spdk_nvmf_dhchap_reply *msg)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	uint8_t response[NVMF_AUTH_DIGEST_MAX_SIZE];
	uint8_t dhsec[NVMF_AUTH_DH_KEY_MAX_SIZE];
	struct spdk_key *key = NULL, *ckey = NULL;
	size_t dhseclen = 0;
	uint8_t hl;
	int rc;

	if (auth->state != NVMF_QPAIR_AUTH_REPLY) {
		AUTH_ERRLOG(qpair, "invalid state=%s\n", nvmf_auth_get_state_name(auth->state));
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
		goto out;
	}
	if (req->length < sizeof(*msg)) {
		AUTH_ERRLOG(qpair, "invalid message length=%"PRIu32"\n", req->length);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		goto out;
	}

	hl = spdk_nvme_dhchap_get_digest_length(auth->digest);
	if (hl == 0 || msg->hl != hl) {
		AUTH_ERRLOG(qpair, "hash length mismatch: %u != %u\n", msg->hl, hl);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		goto out;
	}
	if ((msg->dhvlen % 4) != 0) {
		AUTH_ERRLOG(qpair, "dhvlen=%u is not multiple of 4\n", msg->dhvlen);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		goto out;
	}
	if (req->length != sizeof(*msg) + 2 * hl + msg->dhvlen) {
		AUTH_ERRLOG(qpair, "invalid message length: %"PRIu32" != %zu\n",
			    req->length, sizeof(*msg) + 2 * hl);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		goto out;
	}
	if (msg->t_id != auth->tid) {
		AUTH_ERRLOG(qpair, "transaction id mismatch: %u != %u\n", msg->t_id, auth->tid);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		goto out;
	}
	if (msg->cvalid != 0 && msg->cvalid != 1) {
		AUTH_ERRLOG(qpair, "unexpected cvalid=%d\n", msg->cvalid);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		goto out;
	}
	if (msg->cvalid && msg->seqnum == 0) {
		AUTH_ERRLOG(qpair, "unexpected seqnum=0 with cvalid=1\n");
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		goto out;
	}

	key = nvmf_subsystem_get_dhchap_key(ctrlr->subsys, ctrlr->hostnqn, NVMF_AUTH_KEY_HOST);
	if (key == NULL) {
		AUTH_ERRLOG(qpair, "couldn't get DH-HMAC-CHAP key\n");
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_FAILED);
		goto out;
	}

	if (auth->dhgroup != SPDK_NVMF_DHCHAP_DHGROUP_NULL) {
		AUTH_LOGDUMP("host pubkey:", &msg->rval[2 * hl], msg->dhvlen);
		dhseclen = sizeof(dhsec);
		rc = spdk_nvme_dhchap_dhkey_derive_secret(auth->dhkey, &msg->rval[2 * hl],
				msg->dhvlen, dhsec, &dhseclen);
		if (rc != 0) {
			AUTH_ERRLOG(qpair, "couldn't derive DH secret\n");
			nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_FAILED);
			goto out;
		}

		AUTH_LOGDUMP("dh secret:", dhsec, dhseclen);
	}

	assert(hl <= sizeof(response) && hl <= sizeof(auth->cval));
	rc = spdk_nvme_dhchap_calculate(key, (enum spdk_nvmf_dhchap_hash)auth->digest,
					"HostHost", auth->seqnum, auth->tid, 0,
					ctrlr->hostnqn, ctrlr->subsys->subnqn,
					dhseclen > 0 ? dhsec : NULL, dhseclen,
					auth->cval, response);
	if (rc != 0) {
		AUTH_ERRLOG(qpair, "failed to calculate challenge response: %s\n",
			    spdk_strerror(-rc));
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_FAILED);
		goto out;
	}

	if (memcmp(msg->rval, response, hl) != 0) {
		AUTH_ERRLOG(qpair, "challenge response mismatch\n");
		AUTH_LOGDUMP("response:", msg->rval, hl);
		AUTH_LOGDUMP("expected:", response, hl);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_FAILED);
		goto out;
	}

	if (msg->cvalid) {
		ckey = nvmf_subsystem_get_dhchap_key(ctrlr->subsys, ctrlr->hostnqn,
						     NVMF_AUTH_KEY_CTRLR);
		if (ckey == NULL) {
			AUTH_ERRLOG(qpair, "missing DH-HMAC-CHAP ctrlr key\n");
			nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_FAILED);
			goto out;
		}
		rc = spdk_nvme_dhchap_calculate(ckey, (enum spdk_nvmf_dhchap_hash)auth->digest,
						"Controller", msg->seqnum, auth->tid, 0,
						ctrlr->subsys->subnqn, ctrlr->hostnqn,
						dhseclen > 0 ? dhsec : NULL, dhseclen,
						&msg->rval[hl], auth->cval);
		if (rc != 0) {
			AUTH_ERRLOG(qpair, "failed to calculate ctrlr challenge response: %s\n",
				    spdk_strerror(-rc));
			nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_FAILED);
			goto out;
		}
		auth->cvalid = true;
	}

	if (nvmf_auth_rearm_poller(qpair)) {
		nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, 1);
		nvmf_auth_disconnect_qpair(qpair);
		goto out;
	}

	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_SUCCESS1);
	nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS, 0);
out:
	spdk_keyring_put_key(ckey);
	spdk_keyring_put_key(key);
}

static void
nvmf_auth_success2_exec(struct spdk_nvmf_request *req, struct spdk_nvmf_dhchap_success2 *msg)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;

	if (auth->state != NVMF_QPAIR_AUTH_SUCCESS2) {
		AUTH_ERRLOG(qpair, "invalid state=%s\n", nvmf_auth_get_state_name(auth->state));
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
		return;
	}
	if (req->length != sizeof(*msg)) {
		AUTH_ERRLOG(qpair, "invalid message length=%"PRIu32"\n", req->length);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		return;
	}
	if (msg->t_id != auth->tid) {
		AUTH_ERRLOG(qpair, "transaction id mismatch: %u != %u\n", msg->t_id, auth->tid);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		return;
	}

	AUTH_DEBUGLOG(qpair, "controller authentication successful\n");
	nvmf_qpair_set_state(qpair, SPDK_NVMF_QPAIR_ENABLED);
	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_COMPLETED);
	nvmf_auth_qpair_cleanup(auth);
	nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS, 0);
}

static void
nvmf_auth_failure2_exec(struct spdk_nvmf_request *req, struct spdk_nvmf_auth_failure *msg)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;

	/* AUTH_failure2 is only expected when we're waiting for the success2 message */
	if (auth->state != NVMF_QPAIR_AUTH_SUCCESS2) {
		AUTH_ERRLOG(qpair, "invalid state=%s\n", nvmf_auth_get_state_name(auth->state));
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
		return;
	}
	if (req->length != sizeof(*msg)) {
		AUTH_ERRLOG(qpair, "invalid message length=%"PRIu32"\n", req->length);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		return;
	}
	if (msg->t_id != auth->tid) {
		AUTH_ERRLOG(qpair, "transaction id mismatch: %u != %u\n", msg->t_id, auth->tid);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		return;
	}

	AUTH_ERRLOG(qpair, "ctrlr authentication failed: rc=%d, rce=%d\n", msg->rc, msg->rce);
	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_ERROR);
	nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS, 0);
}

static void
nvmf_auth_send_exec(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_fabric_auth_send_cmd *cmd = &req->cmd->auth_send_cmd;
	struct nvmf_auth_common_header *header;
	int rc;

	rc = nvmf_auth_check_command(req, cmd->secp, cmd->spsp0, cmd->spsp1, cmd->tl);
	if (rc != 0) {
		nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_INVALID_FIELD, 1);
		return;
	}

	header = nvmf_auth_get_message(req, sizeof(*header));
	if (header == NULL) {
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD);
		return;
	}

	switch (header->auth_type) {
	case SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE:
		switch (header->auth_id) {
		case SPDK_NVMF_AUTH_ID_NEGOTIATE:
			nvmf_auth_negotiate_exec(req, (void *)header);
			break;
		case SPDK_NVMF_AUTH_ID_FAILURE2:
			nvmf_auth_failure2_exec(req, (void *)header);
			break;
		default:
			AUTH_ERRLOG(qpair, "unexpected auth_id=%u\n", header->auth_id);
			nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
			break;
		}
		break;
	case SPDK_NVMF_AUTH_TYPE_DHCHAP:
		switch (header->auth_id) {
		case SPDK_NVMF_AUTH_ID_DHCHAP_REPLY:
			nvmf_auth_reply_exec(req, (void *)header);
			break;
		case SPDK_NVMF_AUTH_ID_DHCHAP_SUCCESS2:
			nvmf_auth_success2_exec(req, (void *)header);
			break;
		default:
			AUTH_ERRLOG(qpair, "unexpected auth_id=%u\n", header->auth_id);
			nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
			break;
		}
		break;
	default:
		AUTH_ERRLOG(qpair, "unexpected auth_type=%u\n", header->auth_type);
		nvmf_auth_request_fail1(req, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
		break;
	}
}

static void
nvmf_auth_recv_complete(struct spdk_nvmf_request *req, uint32_t length)
{
	assert(req->cmd->nvmf_cmd.fctype == SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV);
	req->length = length;
	nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_SUCCESS, 0);
}

static void
nvmf_auth_recv_failure1(struct spdk_nvmf_request *req, int fail_reason)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	struct spdk_nvmf_auth_failure *failure;

	failure = nvmf_auth_get_message(req, sizeof(*failure));
	if (failure == NULL) {
		nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_INVALID_FIELD, 1);
		nvmf_auth_disconnect_qpair(qpair);
		return;
	}

	failure->auth_type = SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE;
	failure->auth_id = SPDK_NVMF_AUTH_ID_FAILURE1;
	failure->t_id = auth->tid;
	failure->rc = SPDK_NVMF_AUTH_FAILURE;
	failure->rce = fail_reason;

	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_FAILURE1);
	nvmf_auth_recv_complete(req, sizeof(*failure));
	nvmf_auth_disconnect_qpair(qpair);
}

static int
nvmf_auth_get_seqnum(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_subsystem *subsys = qpair->ctrlr->subsys;
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	int rc;

	pthread_mutex_lock(&subsys->mutex);
	if (subsys->auth_seqnum == 0) {
		rc = RAND_bytes((void *)&subsys->auth_seqnum, sizeof(subsys->auth_seqnum));
		if (rc != 1) {
			pthread_mutex_unlock(&subsys->mutex);
			return -EIO;
		}
	}
	if (++subsys->auth_seqnum == 0) {
		subsys->auth_seqnum = 1;

	}
	auth->seqnum = subsys->auth_seqnum;
	pthread_mutex_unlock(&subsys->mutex);

	return 0;
}

static int
nvmf_auth_recv_challenge(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	struct spdk_nvmf_dhchap_challenge *challenge;
	uint8_t hl, dhv[NVMF_AUTH_DH_KEY_MAX_SIZE];
	size_t dhvlen = 0;
	int rc;

	hl = spdk_nvme_dhchap_get_digest_length(auth->digest);
	assert(hl > 0 && hl <= sizeof(auth->cval));

	if (auth->dhgroup != SPDK_NVMF_DHCHAP_DHGROUP_NULL) {
		auth->dhkey = spdk_nvme_dhchap_generate_dhkey(auth->dhgroup);
		if (auth->dhkey == NULL) {
			AUTH_ERRLOG(qpair, "failed to generate DH key\n");
			return SPDK_NVMF_AUTH_FAILED;
		}

		dhvlen = sizeof(dhv);
		rc = spdk_nvme_dhchap_dhkey_get_pubkey(auth->dhkey, dhv, &dhvlen);
		if (rc != 0) {
			AUTH_ERRLOG(qpair, "failed to get DH public key\n");
			return SPDK_NVMF_AUTH_FAILED;
		}

		AUTH_LOGDUMP("ctrlr pubkey:", dhv, dhvlen);
	}

	challenge = nvmf_auth_get_message(req, sizeof(*challenge) + hl + dhvlen);
	if (challenge == NULL) {
		AUTH_ERRLOG(qpair, "invalid message length: %"PRIu32"\n", req->length);
		return SPDK_NVMF_AUTH_INCORRECT_PAYLOAD;
	}
	rc = nvmf_auth_get_seqnum(qpair);
	if (rc != 0) {
		return SPDK_NVMF_AUTH_FAILED;
	}
	rc = RAND_bytes(auth->cval, hl);
	if (rc != 1) {
		return SPDK_NVMF_AUTH_FAILED;
	}
	if (nvmf_auth_rearm_poller(qpair)) {
		nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, 1);
		nvmf_auth_disconnect_qpair(qpair);
		return 0;
	}

	memcpy(challenge->cval, auth->cval, hl);
	memcpy(&challenge->cval[hl], dhv, dhvlen);
	challenge->auth_type = SPDK_NVMF_AUTH_TYPE_DHCHAP;
	challenge->auth_id = SPDK_NVMF_AUTH_ID_DHCHAP_CHALLENGE;
	challenge->t_id = auth->tid;
	challenge->hl = hl;
	challenge->hash_id = (uint8_t)auth->digest;
	challenge->dhg_id = (uint8_t)auth->dhgroup;
	challenge->dhvlen = dhvlen;
	challenge->seqnum = auth->seqnum;

	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_REPLY);
	nvmf_auth_recv_complete(req, sizeof(*challenge) + hl + dhvlen);

	return 0;
}

static int
nvmf_auth_recv_success1(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	struct spdk_nvmf_dhchap_success1 *success;
	uint8_t hl;

	hl = spdk_nvme_dhchap_get_digest_length(auth->digest);
	success = nvmf_auth_get_message(req, sizeof(*success) + auth->cvalid * hl);
	if (success == NULL) {
		AUTH_ERRLOG(qpair, "invalid message length: %"PRIu32"\n", req->length);
		return SPDK_NVMF_AUTH_INCORRECT_PAYLOAD;
	}

	AUTH_DEBUGLOG(qpair, "host authentication successful\n");
	success->auth_type = SPDK_NVMF_AUTH_TYPE_DHCHAP;
	success->auth_id = SPDK_NVMF_AUTH_ID_DHCHAP_SUCCESS1;
	success->t_id = auth->tid;
	/* Kernel initiator always expects hl to be set, regardless of rvalid */
	success->hl = hl;
	success->rvalid = 0;

	if (!auth->cvalid) {
		/* Host didn't request to authenticate us, we're done */
		nvmf_qpair_set_state(qpair, SPDK_NVMF_QPAIR_ENABLED);
		nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_COMPLETED);
		nvmf_auth_qpair_cleanup(auth);
	} else {
		if (nvmf_auth_rearm_poller(qpair)) {
			nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
						   SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, 1);
			nvmf_auth_disconnect_qpair(qpair);
			return 0;
		}
		AUTH_DEBUGLOG(qpair, "cvalid=1, starting controller authentication\n");
		nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_SUCCESS2);
		memcpy(success->rval, auth->cval, hl);
		success->rvalid = 1;
	}

	nvmf_auth_recv_complete(req, sizeof(*success) + auth->cvalid * hl);
	return 0;
}

static void
nvmf_auth_recv_exec(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	struct spdk_nvmf_fabric_auth_recv_cmd *cmd = &req->cmd->auth_recv_cmd;
	int rc;

	rc = nvmf_auth_check_command(req, cmd->secp, cmd->spsp0, cmd->spsp1, cmd->al);
	if (rc != 0) {
		nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_INVALID_FIELD, 1);
		return;
	}

	spdk_iov_memset(req->iov, req->iovcnt, 0);
	switch (auth->state) {
	case NVMF_QPAIR_AUTH_CHALLENGE:
		rc = nvmf_auth_recv_challenge(req);
		if (rc != 0) {
			nvmf_auth_recv_failure1(req, rc);
		}
		break;
	case NVMF_QPAIR_AUTH_SUCCESS1:
		rc = nvmf_auth_recv_success1(req);
		if (rc != 0) {
			nvmf_auth_recv_failure1(req, rc);
		}
		break;
	case NVMF_QPAIR_AUTH_FAILURE1:
		nvmf_auth_recv_failure1(req, auth->fail_reason);
		break;
	default:
		nvmf_auth_recv_failure1(req, SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE);
		break;
	}
}

static bool
nvmf_auth_check_state(struct spdk_nvmf_qpair *qpair, struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	int rc;

	switch (qpair->state) {
	case SPDK_NVMF_QPAIR_AUTHENTICATING:
		break;
	case SPDK_NVMF_QPAIR_ENABLED:
		if (auth == NULL || auth->state == NVMF_QPAIR_AUTH_COMPLETED) {
			rc = nvmf_qpair_auth_init(qpair);
			if (rc != 0) {
				nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
							   SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, 0);
				return false;
			}
		}
		break;
	default:
		nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR, 0);
		return false;
	}

	return true;
}

int
nvmf_auth_request_exec(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_qpair *qpair = req->qpair;
	union nvmf_h2c_msg *cmd = req->cmd;

	if (!nvmf_auth_check_state(qpair, req)) {
		goto out;
	}

	assert(cmd->nvmf_cmd.opcode == SPDK_NVME_OPC_FABRIC);
	switch (cmd->nvmf_cmd.fctype) {
	case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND:
		nvmf_auth_send_exec(req);
		break;
	case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV:
		nvmf_auth_recv_exec(req);
		break;
	default:
		assert(0 && "invalid fctype");
		nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
					   SPDK_NVME_SC_INTERNAL_DEVICE_ERROR, 0);
		break;
	}
out:
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_qpair_auth_init(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	int rc;

	if (auth == NULL) {
		auth = calloc(1, sizeof(*qpair->auth));
		if (auth == NULL) {
			return -ENOMEM;
		}
	}

	auth->digest = -1;
	qpair->auth = auth;
	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_NEGOTIATE);

	rc = nvmf_auth_rearm_poller(qpair);
	if (rc != 0) {
		AUTH_ERRLOG(qpair, "failed to arm timeout poller: %s\n", spdk_strerror(-rc));
		nvmf_qpair_auth_destroy(qpair);
		return rc;
	}

	return 0;
}

void
nvmf_qpair_auth_destroy(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;

	if (auth != NULL) {
		nvmf_auth_qpair_cleanup(auth);
		free(qpair->auth);
		qpair->auth = NULL;
	}
}

void
nvmf_qpair_auth_dump(struct spdk_nvmf_qpair *qpair, struct spdk_json_write_ctx *w)
{
	struct spdk_nvmf_qpair_auth *auth = qpair->auth;
	const char *digest, *dhgroup;

	if (auth == NULL) {
		return;
	}

	spdk_json_write_named_object_begin(w, "auth");
	spdk_json_write_named_string(w, "state", nvmf_auth_get_state_name(auth->state));
	digest = spdk_nvme_dhchap_get_digest_name(auth->digest);
	spdk_json_write_named_string(w, "digest", digest ? digest : "unknown");
	dhgroup = spdk_nvme_dhchap_get_dhgroup_name(auth->dhgroup);
	spdk_json_write_named_string(w, "dhgroup", dhgroup ? dhgroup : "unknown");
	spdk_json_write_object_end(w);
}

bool
nvmf_auth_is_supported(void)
{
	return true;
}
SPDK_LOG_REGISTER_COMPONENT(nvmf_auth)
