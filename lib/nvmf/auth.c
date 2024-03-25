/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2024 Intel Corporation
 */

#include "spdk/log.h"
#include "spdk/stdinc.h"

#include "nvmf_internal.h"

#define AUTH_ERRLOG(q, fmt, ...) \
	SPDK_ERRLOG("[%s:%s:%u] " fmt, (q)->ctrlr->subsys->subnqn, (q)->ctrlr->hostnqn, \
		    (q)->qid, ## __VA_ARGS__)
#define AUTH_DEBUGLOG(q, fmt, ...) \
	SPDK_DEBUGLOG(nvmf_auth, "[%s:%s:%u] " fmt, \
		      (q)->ctrlr->subsys->subnqn, (q)->ctrlr->hostnqn, (q)->qid, ## __VA_ARGS__)

enum nvmf_qpair_auth_state {
	NVMF_QPAIR_AUTH_NEGOTIATE,
};

struct spdk_nvmf_qpair_auth {
	enum nvmf_qpair_auth_state state;
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

__attribute__((unused)) static const char *
nvmf_auth_get_state_name(enum nvmf_qpair_auth_state state)
{
	static const char *state_names[] = {
		[NVMF_QPAIR_AUTH_NEGOTIATE] = "negotiate",
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

int
nvmf_auth_request_exec(struct spdk_nvmf_request *req)
{
	nvmf_auth_request_complete(req, SPDK_NVME_SCT_GENERIC,
				   SPDK_NVME_SC_INVALID_OPCODE, 1);

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_qpair_auth_init(struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_qpair_auth *auth;

	assert(qpair->auth == NULL);
	auth = calloc(1, sizeof(*qpair->auth));
	if (auth == NULL) {
		return -ENOMEM;
	}

	qpair->auth = auth;
	nvmf_auth_set_state(qpair, NVMF_QPAIR_AUTH_NEGOTIATE);

	return 0;
}

void
nvmf_qpair_auth_destroy(struct spdk_nvmf_qpair *qpair)
{
	free(qpair->auth);
	qpair->auth = NULL;
}

bool
nvmf_auth_is_supported(void)
{
	return true;
}
SPDK_LOG_REGISTER_COMPONENT(nvmf_auth)
