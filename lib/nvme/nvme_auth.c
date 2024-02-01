/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation.  All rights reserved.
 */

#include "spdk/base64.h"
#include "spdk/crc32.h"
#include "spdk/endian.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "nvme_internal.h"
#include <openssl/evp.h>

#define NVME_AUTH_DATA_SIZE		4096
#define NVME_AUTH_CHAP_KEY_MAX_SIZE	256
#define NVME_AUTH_DIGEST_MAX_SIZE	64

#define AUTH_DEBUGLOG(q, fmt, ...) \
	SPDK_DEBUGLOG(nvme_auth, "[%s:%s:%u] " fmt, (q)->ctrlr->trid.subnqn, \
		      (q)->ctrlr->opts.hostnqn, (q)->id, ## __VA_ARGS__)
#define AUTH_ERRLOG(q, fmt, ...) \
	SPDK_ERRLOG("[%s:%s:%u] " fmt, (q)->ctrlr->trid.subnqn, (q)->ctrlr->opts.hostnqn, \
		    (q)->id, ## __VA_ARGS__)

static const char *
nvme_auth_get_digest_name(uint8_t id)
{
	const char *names[] = {
		[SPDK_NVMF_DHCHAP_HASH_SHA256] = "sha256",
		[SPDK_NVMF_DHCHAP_HASH_SHA384] = "sha384",
		[SPDK_NVMF_DHCHAP_HASH_SHA512] = "sha512",
	};

	if (id >= SPDK_COUNTOF(names)) {
		return NULL;
	}

	return names[id];
}

static uint8_t
nvme_auth_get_digest_len(uint8_t id)
{
	uint8_t hlen[] = {
		[SPDK_NVMF_DHCHAP_HASH_SHA256] = 32,
		[SPDK_NVMF_DHCHAP_HASH_SHA384] = 48,
		[SPDK_NVMF_DHCHAP_HASH_SHA512] = 64,
	};

	if (id >= SPDK_COUNTOF(hlen)) {
		return 0;
	}

	return hlen[id];
}

static void
nvme_auth_set_state(struct spdk_nvme_qpair *qpair, enum nvme_qpair_auth_state state)
{
	static const char *state_names[] __attribute__((unused)) = {
		[NVME_QPAIR_AUTH_STATE_NEGOTIATE] = "negotiate",
		[NVME_QPAIR_AUTH_STATE_AWAIT_NEGOTIATE] = "await-negotiate",
		[NVME_QPAIR_AUTH_STATE_AWAIT_CHALLENGE] = "await-challenge",
		[NVME_QPAIR_AUTH_STATE_AWAIT_REPLY] = "await-reply",
		[NVME_QPAIR_AUTH_STATE_AWAIT_SUCCESS1] = "await-success1",
		[NVME_QPAIR_AUTH_STATE_AWAIT_FAILURE2] = "await-failure2",
		[NVME_QPAIR_AUTH_STATE_DONE] = "done",
	};

	AUTH_DEBUGLOG(qpair, "auth state: %s\n", state_names[state]);
	qpair->auth.state = state;
}

static void
nvme_auth_set_failure(struct spdk_nvme_qpair *qpair, int status, bool failure2)
{
	if (qpair->auth.status == 0) {
		qpair->auth.status = status;
	}

	nvme_auth_set_state(qpair, failure2 ?
			    NVME_QPAIR_AUTH_STATE_AWAIT_FAILURE2 :
			    NVME_QPAIR_AUTH_STATE_DONE);
}

static void
nvme_auth_print_cpl(struct spdk_nvme_qpair *qpair, const char *msg)
{
	struct nvme_completion_poll_status *status = qpair->poll_status;

	AUTH_ERRLOG(qpair, "%s failed: sc=%d, sct=%d (timed out: %s)\n", msg, status->cpl.status.sc,
		    status->cpl.status.sct, status->timed_out ? "true" : "false");
}

static int
nvme_auth_transform_key(struct spdk_key *key, int hash, const void *keyin, size_t keylen,
			void *out, size_t outlen)
{
	switch (hash) {
	case SPDK_NVMF_DHCHAP_HASH_NONE:
		if (keylen > outlen) {
			SPDK_ERRLOG("Key buffer too small: %zu < %zu (key=%s)\n", outlen, keylen,
				    spdk_key_get_name(key));
			return -ENOBUFS;
		}
		memcpy(out, keyin, keylen);
		return keylen;
	case SPDK_NVMF_DHCHAP_HASH_SHA256:
	case SPDK_NVMF_DHCHAP_HASH_SHA384:
	case SPDK_NVMF_DHCHAP_HASH_SHA512:
		SPDK_ERRLOG("Key transformation is not supported\n");
		return -EINVAL;
	default:
		SPDK_ERRLOG("Unsupported key hash: 0x%x (key=%s)\n", hash, spdk_key_get_name(key));
		return -EINVAL;
	}
}

static int
nvme_auth_get_key(struct spdk_key *key, void *buf, size_t buflen)
{
	char keystr[NVME_AUTH_CHAP_KEY_MAX_SIZE + 1] = {};
	char keyb64[NVME_AUTH_CHAP_KEY_MAX_SIZE] = {};
	char *tmp, *secret;
	int rc, hash;
	size_t keylen;

	rc = spdk_key_get_key(key, keystr, NVME_AUTH_CHAP_KEY_MAX_SIZE);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to load key=%s: %s\n", spdk_key_get_name(key),
			    spdk_strerror(-rc));
		goto out;
	}

	rc = sscanf(keystr, "DHHC-1:%02x:", &hash);
	if (rc != 1) {
		SPDK_ERRLOG("Invalid key format (key=%s)\n", spdk_key_get_name(key));
		rc = -EINVAL;
		goto out;

	}
	/* Start at the first character after second ":" and remove the trailing ":" */
	secret = &keystr[10];
	tmp = strstr(secret, ":");
	if (!tmp) {
		SPDK_ERRLOG("Invalid key format (key=%s)\n", spdk_key_get_name(key));
		rc = -EINVAL;
		goto out;
	}

	*tmp = '\0';
	keylen = sizeof(keyb64);
	rc = spdk_base64_decode(keyb64, &keylen, secret);
	if (rc != 0) {
		SPDK_ERRLOG("Invalid key format (key=%s)\n", spdk_key_get_name(key));
		rc = -EINVAL;
		goto out;
	}
	/* Only 32B, 48B, and 64B keys are supported (+ 4B, as they're followed by a crc32) */
	if (keylen != 36 && keylen != 52 && keylen != 68) {
		SPDK_ERRLOG("Invalid key size=%zu (key=%s)\n", keylen, spdk_key_get_name(key));
		rc = -EINVAL;
		goto out;
	}

	keylen -= 4;
	if (~spdk_crc32_ieee_update(keyb64, keylen, ~0) != from_le32(&keyb64[keylen])) {
		SPDK_ERRLOG("Invalid key checksum (key=%s)\n", spdk_key_get_name(key));
		rc = -EINVAL;
		goto out;
	}

	rc = nvme_auth_transform_key(key, hash, keyb64, keylen, buf, buflen);
out:
	spdk_memset_s(keystr, sizeof(keystr), 0, sizeof(keystr));
	spdk_memset_s(keyb64, sizeof(keyb64), 0, sizeof(keyb64));

	return rc;
}

static int
nvme_auth_augment_challenge(const void *cval, size_t clen, const void *key, size_t keylen,
			    void *caval, size_t *calen, enum spdk_nvmf_dhchap_hash hash)
{
	EVP_MAC *hmac = NULL;
	EVP_MAC_CTX *ctx = NULL;
	EVP_MD *md = NULL;
	OSSL_PARAM params[2];
	uint8_t keydgst[NVME_AUTH_DIGEST_MAX_SIZE];
	unsigned int dgstlen = sizeof(keydgst);
	int rc = 0;

	/* If there's no key, there's nothing to augment, cval == caval */
	if (key == NULL) {
		assert(clen <= *calen);
		memcpy(caval, cval, clen);
		*calen = clen;
		return 0;
	}

	md = EVP_MD_fetch(NULL, nvme_auth_get_digest_name(hash), NULL);
	if (!md) {
		SPDK_ERRLOG("Failed to fetch digest function: %d\n", hash);
		return -EINVAL;
	}
	if (EVP_Digest(key, keylen, keydgst, &dgstlen, md, NULL) != 1) {
		rc = -EIO;
		goto out;
	}

	hmac = EVP_MAC_fetch(NULL, "hmac", NULL);
	if (hmac == NULL) {
		rc = -EIO;
		goto out;
	}
	ctx = EVP_MAC_CTX_new(hmac);
	if (ctx == NULL) {
		rc = -EIO;
		goto out;
	}
	params[0] = OSSL_PARAM_construct_utf8_string("digest",
			(char *)nvme_auth_get_digest_name(hash), 0);
	params[1] = OSSL_PARAM_construct_end();

	if (EVP_MAC_init(ctx, keydgst, dgstlen, params) != 1) {
		rc = -EIO;
		goto out;
	}
	if (EVP_MAC_update(ctx, cval, clen) != 1) {
		rc = -EIO;
		goto out;
	}
	if (EVP_MAC_final(ctx, caval, calen, *calen) != 1) {
		rc = -EIO;
		goto out;
	}
out:
	EVP_MD_free(md);
	EVP_MAC_CTX_free(ctx);
	EVP_MAC_free(hmac);

	return rc;
}

static int
nvme_auth_calc_response(struct spdk_key *key, enum spdk_nvmf_dhchap_hash hash,
			const char *type, uint32_t seq, uint16_t tid, uint8_t scc,
			const char *nqn1, const char *nqn2, const void *dhkey, size_t dhlen,
			const void *cval, void *rval)
{
	EVP_MAC *hmac;
	EVP_MAC_CTX *ctx;
	OSSL_PARAM params[2];
	uint8_t keybuf[NVME_AUTH_CHAP_KEY_MAX_SIZE], term = 0;
	uint8_t caval[NVME_AUTH_DATA_SIZE];
	size_t hlen, calen = sizeof(caval);
	int rc, keylen;

	hlen = nvme_auth_get_digest_len(hash);
	rc = nvme_auth_augment_challenge(cval, hlen, dhkey, dhlen, caval, &calen, hash);
	if (rc != 0) {
		return rc;
	}

	hmac = EVP_MAC_fetch(NULL, "hmac", NULL);
	if (hmac == NULL) {
		return -EIO;
	}

	ctx = EVP_MAC_CTX_new(hmac);
	if (ctx == NULL) {
		rc = -EIO;
		goto out;
	}

	keylen = nvme_auth_get_key(key, keybuf, sizeof(keybuf));
	if (keylen < 0) {
		rc = keylen;
		goto out;
	}

	params[0] = OSSL_PARAM_construct_utf8_string("digest",
			(char *)nvme_auth_get_digest_name(hash), 0);
	params[1] = OSSL_PARAM_construct_end();

	rc = -EIO;
	if (EVP_MAC_init(ctx, keybuf, (size_t)keylen, params) != 1) {
		goto out;
	}
	if (EVP_MAC_update(ctx, caval, calen) != 1) {
		goto out;
	}
	if (EVP_MAC_update(ctx, (void *)&seq, sizeof(seq)) != 1) {
		goto out;
	}
	if (EVP_MAC_update(ctx, (void *)&tid, sizeof(tid)) != 1) {
		goto out;
	}
	if (EVP_MAC_update(ctx, (void *)&scc, sizeof(scc)) != 1) {
		goto out;
	}
	if (EVP_MAC_update(ctx, (void *)type, strlen(type)) != 1) {
		goto out;
	}
	if (EVP_MAC_update(ctx, (void *)nqn1, strlen(nqn1)) != 1) {
		goto out;
	}
	if (EVP_MAC_update(ctx, (void *)&term, sizeof(term)) != 1) {
		goto out;
	}
	if (EVP_MAC_update(ctx, (void *)nqn2, strlen(nqn2)) != 1) {
		goto out;
	}
	if (EVP_MAC_final(ctx, rval, &hlen, hlen) != 1) {
		goto out;
	}
	rc = 0;
out:
	spdk_memset_s(keybuf, sizeof(keybuf), 0, sizeof(keybuf));
	EVP_MAC_CTX_free(ctx);
	EVP_MAC_free(hmac);

	return rc;
}

static int
nvme_auth_submit_request(struct spdk_nvme_qpair *qpair,
			 enum spdk_nvmf_fabric_cmd_types type, uint32_t len)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct nvme_request *req = qpair->reserved_req;
	struct nvme_completion_poll_status *status = qpair->poll_status;
	struct spdk_nvmf_fabric_auth_recv_cmd rcmd = {};
	struct spdk_nvmf_fabric_auth_send_cmd scmd = {};

	assert(len <= NVME_AUTH_DATA_SIZE);
	memset(&status->cpl, 0, sizeof(status->cpl));
	status->timeout_tsc = ctrlr->opts.admin_timeout_ms * spdk_get_ticks_hz() / 1000 +
			      spdk_get_ticks();
	status->done = false;
	NVME_INIT_REQUEST(req, nvme_completion_poll_cb, status,
			  NVME_PAYLOAD_CONTIG(status->dma_data, NULL), len, 0);
	switch (type) {
	case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND:
		scmd.opcode = SPDK_NVME_OPC_FABRIC;
		scmd.fctype = type;
		scmd.spsp0 = 1;
		scmd.spsp1 = 1;
		scmd.secp = SPDK_NVMF_AUTH_SECP_NVME;
		scmd.tl = len;
		memcpy(&req->cmd, &scmd, sizeof(scmd));
		break;
	case SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV:
		rcmd.opcode = SPDK_NVME_OPC_FABRIC;
		rcmd.fctype = type;
		rcmd.spsp0 = 1;
		rcmd.spsp1 = 1;
		rcmd.secp = SPDK_NVMF_AUTH_SECP_NVME;
		rcmd.al = len;
		memcpy(&req->cmd, &rcmd, sizeof(rcmd));
		break;
	default:
		assert(0 && "invalid command");
		return -EINVAL;
	}

	return nvme_qpair_submit_request(qpair, req);
}

static int
nvme_auth_recv_message(struct spdk_nvme_qpair *qpair)
{
	memset(qpair->poll_status->dma_data, 0, NVME_AUTH_DATA_SIZE);
	return nvme_auth_submit_request(qpair, SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_RECV,
					NVME_AUTH_DATA_SIZE);
}

static bool
nvme_auth_send_failure2(struct spdk_nvme_qpair *qpair, enum spdk_nvmf_auth_failure_reason reason)
{
	struct spdk_nvmf_auth_failure *msg = qpair->poll_status->dma_data;
	struct nvme_auth *auth = &qpair->auth;

	memset(qpair->poll_status->dma_data, 0, NVME_AUTH_DATA_SIZE);
	msg->auth_type = SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE;
	msg->auth_id = SPDK_NVMF_AUTH_ID_FAILURE2;
	msg->t_id = auth->tid;
	msg->rc = SPDK_NVMF_AUTH_FAILURE;
	msg->rce = reason;

	return nvme_auth_submit_request(qpair, SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND,
					sizeof(*msg)) == 0;
}

static int
nvme_auth_check_message(struct spdk_nvme_qpair *qpair, enum spdk_nvmf_auth_id auth_id)
{
	struct spdk_nvmf_auth_failure *msg = qpair->poll_status->dma_data;
	const char *reason = NULL;
	const char *reasons[] = {
		[SPDK_NVMF_AUTH_FAILED] = "authentication failed",
		[SPDK_NVMF_AUTH_PROTOCOL_UNUSABLE] = "protocol not usable",
		[SPDK_NVMF_AUTH_SCC_MISMATCH] = "secure channel concatenation mismatch",
		[SPDK_NVMF_AUTH_HASH_UNUSABLE] = "hash not usable",
		[SPDK_NVMF_AUTH_DHGROUP_UNUSABLE] = "dhgroup not usable",
		[SPDK_NVMF_AUTH_INCORRECT_PAYLOAD] = "incorrect payload",
		[SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE] = "incorrect protocol message",
	};

	switch (msg->auth_type) {
	case SPDK_NVMF_AUTH_TYPE_DHCHAP:
		if (msg->auth_id == auth_id) {
			return 0;
		}
		AUTH_ERRLOG(qpair, "received unexpected DH-HMAC-CHAP message id: %u (expected: %u)\n",
			    msg->auth_id, auth_id);
		break;
	case SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE:
		/* The only common message that we can expect to receive is AUTH_failure1 */
		if (msg->auth_id != SPDK_NVMF_AUTH_ID_FAILURE1) {
			AUTH_ERRLOG(qpair, "received unexpected common message id: %u\n",
				    msg->auth_id);
			break;
		}
		if (msg->rc == SPDK_NVMF_AUTH_FAILURE && msg->rce < SPDK_COUNTOF(reasons)) {
			reason = reasons[msg->rce];
		}
		AUTH_ERRLOG(qpair, "received AUTH_failure1: rc=%d, rce=%d (%s)\n",
			    msg->rc, msg->rce, reason);
		nvme_auth_set_failure(qpair, -EACCES, false);
		return -EACCES;
	default:
		AUTH_ERRLOG(qpair, "received unknown message type: %u\n", msg->auth_type);
		break;
	}

	nvme_auth_set_failure(qpair, -EACCES,
			      nvme_auth_send_failure2(qpair,
					      SPDK_NVMF_AUTH_INCORRECT_PROTOCOL_MESSAGE));
	return -EACCES;
}

static int
nvme_auth_send_negotiate(struct spdk_nvme_qpair *qpair)
{
	struct nvme_auth *auth = &qpair->auth;
	struct spdk_nvmf_auth_negotiate *msg = qpair->poll_status->dma_data;
	struct spdk_nvmf_auth_descriptor *desc = msg->descriptors;
	uint8_t hashids[] = {
		SPDK_NVMF_DHCHAP_HASH_SHA256,
		SPDK_NVMF_DHCHAP_HASH_SHA384,
		SPDK_NVMF_DHCHAP_HASH_SHA512,
	};
	uint8_t dhgids[] = {
		SPDK_NVMF_DHCHAP_DHGROUP_NULL,
	};

	memset(qpair->poll_status->dma_data, 0, NVME_AUTH_DATA_SIZE);
	desc->auth_id = SPDK_NVMF_AUTH_TYPE_DHCHAP;
	desc->halen = SPDK_COUNTOF(hashids);
	desc->dhlen = SPDK_COUNTOF(dhgids);

	assert(desc->halen <= sizeof(desc->hash_id_list));
	assert(desc->dhlen <= sizeof(desc->dhg_id_list));
	memcpy(desc->hash_id_list, hashids, desc->halen);
	memcpy(desc->dhg_id_list, dhgids, desc->dhlen);

	msg->auth_type = SPDK_NVMF_AUTH_TYPE_COMMON_MESSAGE;
	msg->auth_id = SPDK_NVMF_AUTH_ID_NEGOTIATE;
	msg->t_id = auth->tid;
	msg->sc_c = SPDK_NVMF_AUTH_SCC_DISABLED;
	msg->napd = 1;

	return nvme_auth_submit_request(qpair, SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND,
					sizeof(*msg) + msg->napd * sizeof(*desc));
}

static int
nvme_auth_check_challenge(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvmf_dhchap_challenge *challenge = qpair->poll_status->dma_data;
	struct nvme_auth *auth = &qpair->auth;
	uint8_t hl;
	int rc;

	rc = nvme_auth_check_message(qpair, SPDK_NVMF_AUTH_ID_DHCHAP_CHALLENGE);
	if (rc != 0) {
		return rc;
	}

	if (challenge->t_id != auth->tid) {
		AUTH_ERRLOG(qpair, "unexpected tid: received=%u, expected=%u\n",
			    challenge->t_id, auth->tid);
		goto error;
	}

	if (challenge->seqnum == 0) {
		AUTH_ERRLOG(qpair, "received challenge with seqnum=0\n");
		goto error;
	}

	hl = nvme_auth_get_digest_len(challenge->hash_id);
	if (hl == 0) {
		AUTH_ERRLOG(qpair, "unsupported hash function: 0x%x\n", challenge->hash_id);
		goto error;
	}

	if (challenge->hl != hl) {
		AUTH_ERRLOG(qpair, "unexpected hash length: received=%u, expected=%u\n",
			    challenge->hl, hl);
		goto error;
	}

	if (challenge->dhg_id != SPDK_NVMF_DHCHAP_DHGROUP_NULL) {
		AUTH_ERRLOG(qpair, "unsupported dhgroup: 0x%x\n", challenge->dhg_id);
		goto error;
	}

	return 0;
error:
	nvme_auth_set_failure(qpair, -EACCES,
			      nvme_auth_send_failure2(qpair, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD));
	return -EACCES;
}

static int
nvme_auth_send_reply(struct spdk_nvme_qpair *qpair)
{
	struct nvme_completion_poll_status *status = qpair->poll_status;
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct spdk_nvmf_dhchap_challenge *challenge = status->dma_data;
	struct spdk_nvmf_dhchap_reply *reply = status->dma_data;
	struct nvme_auth *auth = &qpair->auth;
	uint8_t hl, response[NVME_AUTH_DATA_SIZE];
	int rc;

	hl = nvme_auth_get_digest_len(challenge->hash_id);
	AUTH_DEBUGLOG(qpair, "key=%s, hash=%u, dhgroup=%u, seq=%u, tid=%u, subnqn=%s, hostnqn=%s, "
		      "len=%u\n", spdk_key_get_name(ctrlr->opts.dhchap_key),
		      challenge->hash_id, challenge->dhg_id, challenge->seqnum, auth->tid,
		      ctrlr->trid.subnqn, ctrlr->opts.hostnqn, hl);
	rc = nvme_auth_calc_response(ctrlr->opts.dhchap_key,
				     (enum spdk_nvmf_dhchap_hash)challenge->hash_id,
				     "HostHost", challenge->seqnum, auth->tid, 0,
				     ctrlr->opts.hostnqn, ctrlr->trid.subnqn, NULL, 0,
				     challenge->cval, response);
	if (rc != 0) {
		AUTH_ERRLOG(qpair, "failed to calculate response: %s\n", spdk_strerror(-rc));
		return rc;
	}

	/* Now that the response has been calculated, send the reply */
	memset(qpair->poll_status->dma_data, 0, NVME_AUTH_DATA_SIZE);
	memcpy(reply->rval, response, hl);

	reply->auth_type = SPDK_NVMF_AUTH_TYPE_DHCHAP;
	reply->auth_id = SPDK_NVMF_AUTH_ID_DHCHAP_REPLY;
	reply->t_id = auth->tid;
	reply->hl = hl;
	reply->cvalid = 0;
	reply->dhvlen = 0;
	reply->seqnum = 0;

	/* The 2 * reply->hl below is because the spec says that both rval[hl] and cval[hl] must
	 * always be part of the reply message, even cvalid is zero.
	 */
	return nvme_auth_submit_request(qpair, SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND,
					sizeof(*reply) + 2 * reply->hl);
}

static int
nvme_auth_check_success1(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvmf_dhchap_success1 *msg = qpair->poll_status->dma_data;
	struct nvme_auth *auth = &qpair->auth;
	int rc;

	rc = nvme_auth_check_message(qpair, SPDK_NVMF_AUTH_ID_DHCHAP_SUCCESS1);
	if (rc != 0) {
		return rc;
	}

	if (msg->t_id != auth->tid) {
		AUTH_ERRLOG(qpair, "unexpected tid: received=%u, expected=%u\n",
			    msg->t_id, auth->tid);
		goto error;
	}

	return 0;
error:
	nvme_auth_set_failure(qpair, -EACCES,
			      nvme_auth_send_failure2(qpair, SPDK_NVMF_AUTH_INCORRECT_PAYLOAD));
	return -EACCES;
}

int
nvme_fabric_qpair_authenticate_poll(struct spdk_nvme_qpair *qpair)
{
	struct nvme_auth *auth = &qpair->auth;
	struct nvme_completion_poll_status *status = qpair->poll_status;
	enum nvme_qpair_auth_state prev_state;
	int rc;

	do {
		prev_state = auth->state;

		switch (auth->state) {
		case NVME_QPAIR_AUTH_STATE_NEGOTIATE:
			rc = nvme_auth_send_negotiate(qpair);
			if (rc != 0) {
				nvme_auth_set_failure(qpair, rc, false);
				AUTH_ERRLOG(qpair, "failed to send AUTH_negotiate: %s\n",
					    spdk_strerror(-rc));
				break;
			}
			nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_AWAIT_NEGOTIATE);
			break;
		case NVME_QPAIR_AUTH_STATE_AWAIT_NEGOTIATE:
			rc = nvme_wait_for_completion_robust_lock_timeout_poll(qpair, status, NULL);
			if (rc != 0) {
				if (rc != -EAGAIN) {
					nvme_auth_print_cpl(qpair, "AUTH_negotiate");
					nvme_auth_set_failure(qpair, rc, false);
				}
				break;
			}
			/* Negotiate has been sent, try to receive the challenge */
			rc = nvme_auth_recv_message(qpair);
			if (rc != 0) {
				nvme_auth_set_failure(qpair, rc, false);
				AUTH_ERRLOG(qpair, "failed to recv DH-HMAC-CHAP_challenge: %s\n",
					    spdk_strerror(-rc));
				break;
			}
			nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_AWAIT_CHALLENGE);
			break;
		case NVME_QPAIR_AUTH_STATE_AWAIT_CHALLENGE:
			rc = nvme_wait_for_completion_robust_lock_timeout_poll(qpair, status, NULL);
			if (rc != 0) {
				if (rc != -EAGAIN) {
					nvme_auth_print_cpl(qpair, "DH-HMAC-CHAP_challenge");
					nvme_auth_set_failure(qpair, rc, false);
				}
				break;
			}
			rc = nvme_auth_check_challenge(qpair);
			if (rc != 0) {
				break;
			}
			rc = nvme_auth_send_reply(qpair);
			if (rc != 0) {
				nvme_auth_set_failure(qpair, rc, false);
				AUTH_ERRLOG(qpair, "failed to send DH-HMAC-CHAP_reply: %s\n",
					    spdk_strerror(-rc));
				break;
			}
			nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_AWAIT_REPLY);
			break;
		case NVME_QPAIR_AUTH_STATE_AWAIT_REPLY:
			rc = nvme_wait_for_completion_robust_lock_timeout_poll(qpair, status, NULL);
			if (rc != 0) {
				if (rc != -EAGAIN) {
					nvme_auth_print_cpl(qpair, "DH-HMAC-CHAP_reply");
					nvme_auth_set_failure(qpair, rc, false);
				}
				break;
			}
			/* Reply has been sent, try to receive response */
			rc = nvme_auth_recv_message(qpair);
			if (rc != 0) {
				nvme_auth_set_failure(qpair, rc, false);
				AUTH_ERRLOG(qpair, "failed to recv DH-HMAC-CHAP_success1: %s\n",
					    spdk_strerror(-rc));
				break;
			}
			nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_AWAIT_SUCCESS1);
			break;
		case NVME_QPAIR_AUTH_STATE_AWAIT_SUCCESS1:
			rc = nvme_wait_for_completion_robust_lock_timeout_poll(qpair, status, NULL);
			if (rc != 0) {
				if (rc != -EAGAIN) {
					nvme_auth_print_cpl(qpair, "DH-HMAC-CHAP_success1");
					nvme_auth_set_failure(qpair, rc, false);
				}
				break;
			}
			rc = nvme_auth_check_success1(qpair);
			if (rc != 0) {
				break;
			}
			AUTH_DEBUGLOG(qpair, "authentication completed successfully\n");
			nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_DONE);
			break;
		case NVME_QPAIR_AUTH_STATE_AWAIT_FAILURE2:
			rc = nvme_wait_for_completion_robust_lock_timeout_poll(qpair, status, NULL);
			if (rc == -EAGAIN) {
				break;
			}
			nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_DONE);
			break;
		case NVME_QPAIR_AUTH_STATE_DONE:
			if (qpair->poll_status != NULL && !status->timed_out) {
				qpair->poll_status = NULL;
				spdk_free(status->dma_data);
				free(status);
			}
			return auth->status;
		default:
			assert(0 && "invalid state");
			return -EINVAL;
		}
	} while (auth->state != prev_state);

	return -EAGAIN;
}

int
nvme_fabric_qpair_authenticate_async(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct nvme_completion_poll_status *status;
	struct nvme_auth *auth = &qpair->auth;
	int rc;

	if (ctrlr->opts.dhchap_key == NULL) {
		AUTH_ERRLOG(qpair, "missing DH-HMAC-CHAP key\n");
		return -ENOKEY;
	}

	if (qpair->auth.flags & NVME_QPAIR_AUTH_FLAG_ASCR) {
		AUTH_ERRLOG(qpair, "secure channel concatentation is not supported\n");
		return -EINVAL;
	}

	status = calloc(1, sizeof(*qpair->poll_status));
	if (!status) {
		AUTH_ERRLOG(qpair, "failed to allocate poll status\n");
		return -ENOMEM;
	}

	status->dma_data = spdk_zmalloc(NVME_AUTH_DATA_SIZE, 0, NULL, SPDK_ENV_LCORE_ID_ANY,
					SPDK_MALLOC_DMA);
	if (!status->dma_data) {
		AUTH_ERRLOG(qpair, "failed to allocate poll status\n");
		free(status);
		return -ENOMEM;
	}

	assert(qpair->poll_status == NULL);
	qpair->poll_status = status;

	nvme_robust_mutex_lock(&ctrlr->ctrlr_lock);
	auth->tid = ctrlr->auth_tid++;
	nvme_robust_mutex_unlock(&ctrlr->ctrlr_lock);

	nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_NEGOTIATE);

	/* Do the initial poll to kick-start the state machine */
	rc = nvme_fabric_qpair_authenticate_poll(qpair);
	return rc != -EAGAIN ? rc : 0;
}
SPDK_LOG_REGISTER_COMPONENT(nvme_auth)
