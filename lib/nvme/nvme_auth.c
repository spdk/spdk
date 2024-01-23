/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation.  All rights reserved.
 */

#include "spdk/log.h"
#include "spdk/string.h"
#include "nvme_internal.h"

#define NVME_AUTH_DATA_SIZE 4096

#define AUTH_DEBUGLOG(q, fmt, ...) \
	SPDK_DEBUGLOG(nvme_auth, "[%s:%s:%u] " fmt, (q)->ctrlr->trid.subnqn, \
		      (q)->ctrlr->opts.hostnqn, (q)->id, ## __VA_ARGS__)
#define AUTH_ERRLOG(q, fmt, ...) \
	SPDK_ERRLOG("[%s:%s:%u] " fmt, (q)->ctrlr->trid.subnqn, (q)->ctrlr->opts.hostnqn, \
		    (q)->id, ## __VA_ARGS__)

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
			nvme_auth_set_state(qpair, NVME_QPAIR_AUTH_STATE_AWAIT_CHALLENGE);
			break;
		case NVME_QPAIR_AUTH_STATE_AWAIT_CHALLENGE:
		case NVME_QPAIR_AUTH_STATE_AWAIT_REPLY:
		case NVME_QPAIR_AUTH_STATE_AWAIT_SUCCESS1:
		case NVME_QPAIR_AUTH_STATE_AWAIT_FAILURE2:
			nvme_auth_set_failure(qpair, -ENOTSUP, false);
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
