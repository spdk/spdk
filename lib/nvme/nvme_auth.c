/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation.  All rights reserved.
 */

#include "spdk/base64.h"
#include "spdk/crc32.h"
#include "spdk/endian.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk_internal/nvme.h"
#include "nvme_internal.h"

#ifdef SPDK_CONFIG_HAVE_EVP_MAC
#include <openssl/dh.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#endif

struct nvme_auth_digest {
	uint8_t		id;
	const char	*name;
	uint8_t		len;
};

struct nvme_auth_dhgroup {
	uint8_t		id;
	const char	*name;
};

#define NVME_AUTH_DATA_SIZE			4096
#define NVME_AUTH_DH_KEY_MAX_SIZE		1024
#define NVME_AUTH_CHAP_KEY_MAX_SIZE		256

#define AUTH_DEBUGLOG(q, fmt, ...) \
	SPDK_DEBUGLOG(nvme_auth, "[%s:%s:%u] " fmt, (q)->ctrlr->trid.subnqn, \
		      (q)->ctrlr->opts.hostnqn, (q)->id, ## __VA_ARGS__)
#define AUTH_ERRLOG(q, fmt, ...) \
	SPDK_ERRLOG("[%s:%s:%u] " fmt, (q)->ctrlr->trid.subnqn, (q)->ctrlr->opts.hostnqn, \
		    (q)->id, ## __VA_ARGS__)
#define AUTH_LOGDUMP(msg, buf, len) \
	SPDK_LOGDUMP(nvme_auth, msg, buf, len)

static const struct nvme_auth_digest g_digests[] = {
	{ SPDK_NVMF_DHCHAP_HASH_SHA256, "sha256", 32 },
	{ SPDK_NVMF_DHCHAP_HASH_SHA384, "sha384", 48 },
	{ SPDK_NVMF_DHCHAP_HASH_SHA512, "sha512", 64 },
};

static const struct nvme_auth_dhgroup g_dhgroups[] = {
	{ SPDK_NVMF_DHCHAP_DHGROUP_NULL, "null" },
	{ SPDK_NVMF_DHCHAP_DHGROUP_2048, "ffdhe2048" },
	{ SPDK_NVMF_DHCHAP_DHGROUP_3072, "ffdhe3072" },
	{ SPDK_NVMF_DHCHAP_DHGROUP_4096, "ffdhe4096" },
	{ SPDK_NVMF_DHCHAP_DHGROUP_6144, "ffdhe6144" },
	{ SPDK_NVMF_DHCHAP_DHGROUP_8192, "ffdhe8192" },
};

static const struct nvme_auth_digest *
nvme_auth_get_digest(int id)
{
	size_t i;

	for (i = 0; i < SPDK_COUNTOF(g_digests); ++i) {
		if (g_digests[i].id == id) {
			return &g_digests[i];
		}
	}

	return NULL;
}

int
spdk_nvme_dhchap_get_digest_id(const char *digest)
{
	size_t i;

	for (i = 0; i < SPDK_COUNTOF(g_digests); ++i) {
		if (strcmp(g_digests[i].name, digest) == 0) {
			return g_digests[i].id;
		}
	}

	return -EINVAL;
}

const char *
spdk_nvme_dhchap_get_digest_name(int id)
{
	const struct nvme_auth_digest *digest = nvme_auth_get_digest(id);

	return digest != NULL ? digest->name : NULL;
}

int
spdk_nvme_dhchap_get_dhgroup_id(const char *dhgroup)
{
	size_t i;

	for (i = 0; i < SPDK_COUNTOF(g_dhgroups); ++i) {
		if (strcmp(g_dhgroups[i].name, dhgroup) == 0) {
			return g_dhgroups[i].id;
		}
	}

	return -EINVAL;
}

const char *
spdk_nvme_dhchap_get_dhgroup_name(int id)
{
	size_t i;

	for (i = 0; i < SPDK_COUNTOF(g_dhgroups); ++i) {
		if (g_dhgroups[i].id == id) {
			return g_dhgroups[i].name;
		}
	}

	return NULL;
}

uint8_t
spdk_nvme_dhchap_get_digest_length(int id)
{
	const struct nvme_auth_digest *digest = nvme_auth_get_digest(id);

	return digest != NULL ? digest->len : 0;
}

#ifdef SPDK_CONFIG_HAVE_EVP_MAC
static bool
nvme_auth_digest_allowed(struct spdk_nvme_qpair *qpair, uint8_t digest)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;

	return ctrlr->opts.dhchap_digests & SPDK_BIT(digest);
}

static bool
nvme_auth_dhgroup_allowed(struct spdk_nvme_qpair *qpair, uint8_t dhgroup)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;

	return ctrlr->opts.dhchap_dhgroups & SPDK_BIT(dhgroup);
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
		[NVME_QPAIR_AUTH_STATE_AWAIT_SUCCESS2] = "await-success2",
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

static uint32_t
nvme_auth_get_seqnum(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	uint32_t seqnum;
	int rc;

	nvme_ctrlr_lock(ctrlr);
	if (ctrlr->auth_seqnum == 0) {
		rc = RAND_bytes((void *)&ctrlr->auth_seqnum, sizeof(ctrlr->auth_seqnum));
		if (rc != 1) {
			nvme_ctrlr_unlock(ctrlr);
			return 0;
		}
	}
	if (++ctrlr->auth_seqnum == 0) {
		ctrlr->auth_seqnum = 1;
	}
	seqnum = ctrlr->auth_seqnum;
	nvme_ctrlr_unlock(ctrlr);

	return seqnum;
}

static int
nvme_auth_transform_key(struct spdk_key *key, int hash, const char *nqn,
			const void *keyin, size_t keylen, void *out, size_t outlen)
{
	EVP_MAC *hmac = NULL;
	EVP_MAC_CTX *ctx = NULL;
	OSSL_PARAM params[2];
	int rc;

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
		break;
	default:
		SPDK_ERRLOG("Unsupported key hash: 0x%x (key=%s)\n", hash, spdk_key_get_name(key));
		return -EINVAL;
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
	params[0] = OSSL_PARAM_construct_utf8_string("digest",
			(char *)spdk_nvme_dhchap_get_digest_name(hash), 0);
	params[1] = OSSL_PARAM_construct_end();

	if (EVP_MAC_init(ctx, keyin, keylen, params) != 1) {
		rc = -EIO;
		goto out;
	}
	if (EVP_MAC_update(ctx, nqn, strlen(nqn)) != 1) {
		rc = -EIO;
		goto out;
	}
	if (EVP_MAC_update(ctx, "NVMe-over-Fabrics", strlen("NVMe-over-Fabrics")) != 1) {
		rc = -EIO;
		goto out;
	}
	if (EVP_MAC_final(ctx, out, &outlen, outlen) != 1) {
		rc = -EIO;
		goto out;
	}
	rc = (int)outlen;
out:
	EVP_MAC_CTX_free(ctx);
	EVP_MAC_free(hmac);

	return rc;
}

static int
nvme_auth_get_key(struct spdk_key *key, const char *nqn, void *buf, size_t buflen)
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

	rc = nvme_auth_transform_key(key, hash, nqn, keyb64, keylen, buf, buflen);
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

	md = EVP_MD_fetch(NULL, spdk_nvme_dhchap_get_digest_name(hash), NULL);
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
			(char *)spdk_nvme_dhchap_get_digest_name(hash), 0);
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

int
spdk_nvme_dhchap_calculate(struct spdk_key *key, enum spdk_nvmf_dhchap_hash hash,
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

	hlen = spdk_nvme_dhchap_get_digest_length(hash);
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

	keylen = nvme_auth_get_key(key, nqn1, keybuf, sizeof(keybuf));
	if (keylen < 0) {
		rc = keylen;
		goto out;
	}

	params[0] = OSSL_PARAM_construct_utf8_string("digest",
			(char *)spdk_nvme_dhchap_get_digest_name(hash), 0);
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

struct spdk_nvme_dhchap_dhkey *
spdk_nvme_dhchap_generate_dhkey(enum spdk_nvmf_dhchap_dhgroup dhgroup)
{
	EVP_PKEY_CTX *ctx = NULL;
	EVP_PKEY *key = NULL;
	OSSL_PARAM params[2];

	ctx = EVP_PKEY_CTX_new_from_name(NULL, "DHX", NULL);
	if (ctx == NULL) {
		goto error;
	}
	if (EVP_PKEY_keygen_init(ctx) != 1) {
		goto error;
	}

	params[0] = OSSL_PARAM_construct_utf8_string("group",
			(char *)spdk_nvme_dhchap_get_dhgroup_name(dhgroup), 0);
	params[1] = OSSL_PARAM_construct_end();
	if (EVP_PKEY_CTX_set_params(ctx, params) != 1) {
		SPDK_ERRLOG("Failed to set dhkey's dhgroup: %s\n",
			    spdk_nvme_dhchap_get_dhgroup_name(dhgroup));
		goto error;
	}
	if (EVP_PKEY_generate(ctx, &key) != 1) {
		goto error;
	}
error:
	EVP_PKEY_CTX_free(ctx);
	return (void *)key;
}

void
spdk_nvme_dhchap_dhkey_free(struct spdk_nvme_dhchap_dhkey **key)
{
	if (key == NULL) {
		return;
	}

	EVP_PKEY_free(*(EVP_PKEY **)key);
	*key = NULL;
}

int
spdk_nvme_dhchap_dhkey_get_pubkey(struct spdk_nvme_dhchap_dhkey *dhkey, void *pub, size_t *len)
{
	EVP_PKEY *key = (EVP_PKEY *)dhkey;
	BIGNUM *bn = NULL;
	int rc;
	const size_t num_bytes = (size_t)spdk_divide_round_up(EVP_PKEY_get_bits(key), 8);

	if (num_bytes == 0) {
		SPDK_ERRLOG("Failed to get key size\n");
		return -EIO;
	}

	if (num_bytes > *len) {
		SPDK_ERRLOG("Insufficient key buffer size=%zu (needed=%zu)",
			    *len, num_bytes);
		return -EINVAL;
	}
	*len = num_bytes;

	if (EVP_PKEY_get_bn_param(key, "pub", &bn) != 1) {
		rc = -EIO;
		goto error;
	}

	rc = BN_bn2binpad(bn, pub, *len);
	if (rc <= 0) {
		rc = -EIO;
		goto error;
	}
	rc = 0;
error:
	BN_free(bn);
	return rc;
}

static EVP_PKEY *
nvme_auth_get_peerkey(const void *peerkey, size_t len, const char *dhgroup)
{
	EVP_PKEY_CTX *ctx = NULL;
	EVP_PKEY *result = NULL, *key = NULL;
	OSSL_PARAM_BLD *bld = NULL;
	OSSL_PARAM *params = NULL;
	BIGNUM *bn = NULL;

	ctx = EVP_PKEY_CTX_new_from_name(NULL, "DHX", NULL);
	if (ctx == NULL) {
		goto error;
	}
	if (EVP_PKEY_fromdata_init(ctx) != 1) {
		goto error;
	}

	bn = BN_bin2bn(peerkey, len, NULL);
	if (bn == NULL) {
		goto error;
	}

	bld = OSSL_PARAM_BLD_new();
	if (bld == NULL) {
		goto error;
	}
	if (OSSL_PARAM_BLD_push_BN(bld, "pub", bn) != 1) {
		goto error;
	}
	if (OSSL_PARAM_BLD_push_utf8_string(bld, "group", dhgroup, 0) != 1) {
		goto error;
	}

	params = OSSL_PARAM_BLD_to_param(bld);
	if (params == NULL) {
		goto error;
	}
	if (EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, params) != 1) {
		SPDK_ERRLOG("Failed to create dhkey peer key\n");
		goto error;
	}

	result = EVP_PKEY_dup(key);
error:
	EVP_PKEY_free(key);
	EVP_PKEY_CTX_free(ctx);
	OSSL_PARAM_BLD_free(bld);
	OSSL_PARAM_free(params);
	BN_free(bn);

	return result;
}

int
spdk_nvme_dhchap_dhkey_derive_secret(struct spdk_nvme_dhchap_dhkey *dhkey,
				     const void *peer, size_t peerlen, void *secret, size_t *seclen)
{
	EVP_PKEY *key = (EVP_PKEY *)dhkey;
	EVP_PKEY_CTX *ctx = NULL;
	EVP_PKEY *peerkey = NULL;
	char dhgroup[64] = {};
	int rc = 0;

	if (EVP_PKEY_get_utf8_string_param(key, "group", dhgroup,
					   sizeof(dhgroup), NULL) != 1) {
		return -EIO;
	}
	peerkey = nvme_auth_get_peerkey(peer, peerlen, dhgroup);
	if (peerkey == NULL) {
		return -EINVAL;
	}
	ctx = EVP_PKEY_CTX_new(key, NULL);
	if (ctx == NULL) {
		rc = -ENOMEM;
		goto out;
	}
	if (EVP_PKEY_derive_init(ctx) != 1) {
		rc = -EIO;
		goto out;
	}
	if (EVP_PKEY_CTX_set_dh_pad(ctx, 1) <= 0) {
		rc = -EIO;
		goto out;
	}
	if (EVP_PKEY_derive_set_peer(ctx, peerkey) != 1) {
		SPDK_ERRLOG("Failed to set dhsecret's peer key\n");
		rc = -EINVAL;
		goto out;
	}
	if (EVP_PKEY_derive(ctx, secret, seclen) != 1) {
		SPDK_ERRLOG("Failed to derive dhsecret\n");
		rc = -ENOBUFS;
		goto out;
	}
out:
	EVP_PKEY_free(peerkey);
	EVP_PKEY_CTX_free(ctx);

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
	size_t i;

	memset(qpair->poll_status->dma_data, 0, NVME_AUTH_DATA_SIZE);
	desc->auth_id = SPDK_NVMF_AUTH_TYPE_DHCHAP;
	assert(SPDK_COUNTOF(g_digests) <= sizeof(desc->hash_id_list));
	assert(SPDK_COUNTOF(g_dhgroups) <= sizeof(desc->dhg_id_list));

	for (i = 0; i < SPDK_COUNTOF(g_digests); ++i) {
		if (!nvme_auth_digest_allowed(qpair, g_digests[i].id)) {
			continue;
		}
		AUTH_DEBUGLOG(qpair, "digest: %u (%s)\n", g_digests[i].id,
			      spdk_nvme_dhchap_get_digest_name(g_digests[i].id));
		desc->hash_id_list[desc->halen++] = g_digests[i].id;
	}
	for (i = 0; i < SPDK_COUNTOF(g_dhgroups); ++i) {
		if (!nvme_auth_dhgroup_allowed(qpair, g_dhgroups[i].id)) {
			continue;
		}
		AUTH_DEBUGLOG(qpair, "dhgroup: %u (%s)\n", g_dhgroups[i].id,
			      spdk_nvme_dhchap_get_dhgroup_name(g_dhgroups[i].id));
		desc->dhg_id_list[desc->dhlen++] = g_dhgroups[i].id;
	}

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

	hl = spdk_nvme_dhchap_get_digest_length(challenge->hash_id);
	if (hl == 0) {
		AUTH_ERRLOG(qpair, "unsupported hash function: 0x%x\n", challenge->hash_id);
		goto error;
	}

	if (challenge->hl != hl) {
		AUTH_ERRLOG(qpair, "unexpected hash length: received=%u, expected=%u\n",
			    challenge->hl, hl);
		goto error;
	}

	switch (challenge->dhg_id) {
	case SPDK_NVMF_DHCHAP_DHGROUP_NULL:
		if (challenge->dhvlen != 0) {
			AUTH_ERRLOG(qpair, "unexpected dhvlen=%u for dhgroup 0\n",
				    challenge->dhvlen);
			goto error;
		}
		break;
	case SPDK_NVMF_DHCHAP_DHGROUP_2048:
	case SPDK_NVMF_DHCHAP_DHGROUP_3072:
	case SPDK_NVMF_DHCHAP_DHGROUP_4096:
	case SPDK_NVMF_DHCHAP_DHGROUP_6144:
	case SPDK_NVMF_DHCHAP_DHGROUP_8192:
		if (sizeof(*challenge) + hl + challenge->dhvlen > NVME_AUTH_DATA_SIZE ||
		    challenge->dhvlen == 0) {
			AUTH_ERRLOG(qpair, "invalid dhvlen=%u for dhgroup %u\n",
				    challenge->dhvlen, challenge->dhg_id);
			goto error;
		}
		break;
	default:
		AUTH_ERRLOG(qpair, "unsupported dhgroup: 0x%x\n", challenge->dhg_id);
		goto error;
	}

	if (!nvme_auth_digest_allowed(qpair, challenge->hash_id)) {
		AUTH_ERRLOG(qpair, "received disallowed digest: %u (%s)\n", challenge->hash_id,
			    spdk_nvme_dhchap_get_digest_name(challenge->hash_id));
		goto error;
	}

	if (!nvme_auth_dhgroup_allowed(qpair, challenge->dhg_id)) {
		AUTH_ERRLOG(qpair, "received disallowed dhgroup: %u (%s)\n", challenge->dhg_id,
			    spdk_nvme_dhchap_get_dhgroup_name(challenge->dhg_id));
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
	struct spdk_nvme_dhchap_dhkey *dhkey;
	uint8_t hl, response[NVME_AUTH_DATA_SIZE];
	uint8_t pubkey[NVME_AUTH_DH_KEY_MAX_SIZE];
	uint8_t dhsec[NVME_AUTH_DH_KEY_MAX_SIZE];
	uint8_t ctrlr_challenge[NVME_AUTH_DIGEST_MAX_SIZE] = {};
	size_t dhseclen = 0, publen = 0;
	uint32_t seqnum = 0;
	int rc;

	auth->hash = challenge->hash_id;
	hl = spdk_nvme_dhchap_get_digest_length(challenge->hash_id);
	if (challenge->dhg_id != SPDK_NVMF_DHCHAP_DHGROUP_NULL) {
		dhseclen = sizeof(dhsec);
		publen = sizeof(pubkey);
		AUTH_LOGDUMP("ctrlr pubkey:", &challenge->cval[hl], challenge->dhvlen);
		dhkey = spdk_nvme_dhchap_generate_dhkey(
				(enum spdk_nvmf_dhchap_dhgroup)challenge->dhg_id);
		if (dhkey == NULL) {
			return -EINVAL;
		}
		rc = spdk_nvme_dhchap_dhkey_get_pubkey(dhkey, pubkey, &publen);
		if (rc != 0) {
			spdk_nvme_dhchap_dhkey_free(&dhkey);
			return rc;
		}
		AUTH_LOGDUMP("host pubkey:", pubkey, publen);
		rc = spdk_nvme_dhchap_dhkey_derive_secret(dhkey,
				&challenge->cval[hl], challenge->dhvlen, dhsec, &dhseclen);
		spdk_nvme_dhchap_dhkey_free(&dhkey);
		if (rc != 0) {
			return rc;
		}

		AUTH_LOGDUMP("dh secret:", dhsec, dhseclen);
	}

	AUTH_DEBUGLOG(qpair, "key=%s, hash=%u, dhgroup=%u, seq=%u, tid=%u, subnqn=%s, hostnqn=%s, "
		      "len=%u\n", spdk_key_get_name(ctrlr->opts.dhchap_key),
		      challenge->hash_id, challenge->dhg_id, challenge->seqnum, auth->tid,
		      ctrlr->trid.subnqn, ctrlr->opts.hostnqn, hl);
	rc = spdk_nvme_dhchap_calculate(ctrlr->opts.dhchap_key,
					(enum spdk_nvmf_dhchap_hash)challenge->hash_id,
					"HostHost", challenge->seqnum, auth->tid, 0,
					ctrlr->opts.hostnqn, ctrlr->trid.subnqn,
					dhseclen > 0 ? dhsec : NULL, dhseclen,
					challenge->cval, response);
	if (rc != 0) {
		AUTH_ERRLOG(qpair, "failed to calculate response: %s\n", spdk_strerror(-rc));
		return rc;
	}

	if (ctrlr->opts.dhchap_ctrlr_key != NULL) {
		seqnum = nvme_auth_get_seqnum(qpair);
		if (seqnum == 0) {
			return -EIO;
		}

		assert(sizeof(ctrlr_challenge) >= hl);
		rc = RAND_bytes(ctrlr_challenge, hl);
		if (rc != 1) {
			return -EIO;
		}

		rc = spdk_nvme_dhchap_calculate(ctrlr->opts.dhchap_ctrlr_key,
						(enum spdk_nvmf_dhchap_hash)challenge->hash_id,
						"Controller", seqnum, auth->tid, 0,
						ctrlr->trid.subnqn, ctrlr->opts.hostnqn,
						dhseclen > 0 ? dhsec : NULL, dhseclen,
						ctrlr_challenge, auth->challenge);
		if (rc != 0) {
			AUTH_ERRLOG(qpair, "failed to calculate controller's response: %s\n",
				    spdk_strerror(-rc));
			return rc;
		}
	}

	/* Now that the response has been calculated, send the reply */
	memset(qpair->poll_status->dma_data, 0, NVME_AUTH_DATA_SIZE);
	assert(sizeof(*reply) + 2 * hl + publen <= NVME_AUTH_DATA_SIZE);
	memcpy(reply->rval, response, hl);
	memcpy(&reply->rval[1 * hl], ctrlr_challenge, hl);
	memcpy(&reply->rval[2 * hl], pubkey, publen);

	reply->auth_type = SPDK_NVMF_AUTH_TYPE_DHCHAP;
	reply->auth_id = SPDK_NVMF_AUTH_ID_DHCHAP_REPLY;
	reply->t_id = auth->tid;
	reply->hl = hl;
	reply->cvalid = ctrlr->opts.dhchap_ctrlr_key != NULL;
	reply->dhvlen = publen;
	reply->seqnum = seqnum;

	/* The 2 * reply->hl below is because the spec says that both rval[hl] and cval[hl] must
	 * always be part of the reply message, even cvalid is zero.
	 */
	return nvme_auth_submit_request(qpair, SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND,
					sizeof(*reply) + 2 * reply->hl + publen);
}

static int
nvme_auth_check_success1(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvmf_dhchap_success1 *msg = qpair->poll_status->dma_data;
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct nvme_auth *auth = &qpair->auth;
	uint8_t hl;
	int rc, status;

	rc = nvme_auth_check_message(qpair, SPDK_NVMF_AUTH_ID_DHCHAP_SUCCESS1);
	if (rc != 0) {
		return rc;
	}

	if (msg->t_id != auth->tid) {
		AUTH_ERRLOG(qpair, "unexpected tid: received=%u, expected=%u\n",
			    msg->t_id, auth->tid);
		status = SPDK_NVMF_AUTH_INCORRECT_PAYLOAD;
		goto error;
	}

	if (ctrlr->opts.dhchap_ctrlr_key != NULL) {
		if (!msg->rvalid) {
			AUTH_ERRLOG(qpair, "received rvalid=0, expected response\n");
			status = SPDK_NVMF_AUTH_INCORRECT_PAYLOAD;
			goto error;
		}

		hl = spdk_nvme_dhchap_get_digest_length(auth->hash);
		if (msg->hl != hl) {
			AUTH_ERRLOG(qpair, "received invalid hl=%u, expected=%u\n", msg->hl, hl);
			status = SPDK_NVMF_AUTH_INCORRECT_PAYLOAD;
			goto error;
		}

		if (memcmp(msg->rval, auth->challenge, hl) != 0) {
			AUTH_ERRLOG(qpair, "controller challenge mismatch\n");
			AUTH_LOGDUMP("received:", msg->rval, hl);
			AUTH_LOGDUMP("expected:", auth->challenge, hl);
			status = SPDK_NVMF_AUTH_FAILED;
			goto error;
		}
	}

	return 0;
error:
	nvme_auth_set_failure(qpair, -EACCES, nvme_auth_send_failure2(qpair, status));

	return -EACCES;
}

static int
nvme_auth_send_success2(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvmf_dhchap_success2 *msg = qpair->poll_status->dma_data;
	struct nvme_auth *auth = &qpair->auth;

	memset(qpair->poll_status->dma_data, 0, NVME_AUTH_DATA_SIZE);
	msg->auth_type = SPDK_NVMF_AUTH_TYPE_DHCHAP;
	msg->auth_id = SPDK_NVMF_AUTH_ID_DHCHAP_SUCCESS2;
	msg->t_id = auth->tid;

	return nvme_auth_submit_request(qpair, SPDK_NVMF_FABRIC_COMMAND_AUTHENTICATION_SEND,
					sizeof(*msg));
}

int
nvme_fabric_qpair_authenticate_poll(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
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
			if (ctrlr->opts.dhchap_ctrlr_key != NULL) {
				rc = nvme_auth_send_success2(qpair);
				if (rc != 0) {
					AUTH_ERRLOG(qpair, "failed to send DH-HMAC-CHAP_success2: "
						    "%s\n", spdk_strerror(rc));
					nvme_auth_set_failure(qpair, rc, false);
					break;
				}
				nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_AWAIT_SUCCESS2);
				break;
			}
			nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_DONE);
			break;
		case NVME_QPAIR_AUTH_STATE_AWAIT_SUCCESS2:
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

	nvme_ctrlr_lock(ctrlr);
	auth->tid = ctrlr->auth_tid++;
	nvme_ctrlr_unlock(ctrlr);

	nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_NEGOTIATE);

	/* Do the initial poll to kick-start the state machine */
	rc = nvme_fabric_qpair_authenticate_poll(qpair);
	return rc != -EAGAIN ? rc : 0;
}
#endif /* SPDK_CONFIG_EVP_MAC */

SPDK_LOG_REGISTER_COMPONENT(nvme_auth)
