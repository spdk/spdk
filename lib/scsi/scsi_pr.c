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

#include "scsi_internal.h"

#include "spdk/endian.h"

/* Get registrant by I_T nexus */
static struct spdk_scsi_pr_registrant *
spdk_scsi_pr_get_registrant(struct spdk_scsi_lun *lun,
			    struct spdk_scsi_port *initiator_port,
			    struct spdk_scsi_port *target_port)
{
	struct spdk_scsi_pr_registrant *reg, *tmp;

	TAILQ_FOREACH_SAFE(reg, &lun->reg_head, link, tmp) {
		if (initiator_port == reg->initiator_port &&
		    target_port == reg->target_port) {
			return reg;
		}
	}

	return NULL;
}

/* Reservation type is all registrants or not */
static inline bool
spdk_scsi_pr_is_all_registrants_type(struct spdk_scsi_lun *lun)
{
	return (lun->reservation.rtype == SPDK_SCSI_PR_WRITE_EXCLUSIVE_ALL_REGS ||
		lun->reservation.rtype == SPDK_SCSI_PR_EXCLUSIVE_ACCESS_ALL_REGS);
}

/* Registrant is reservation holder or not */
static inline bool
spdk_scsi_pr_registrant_is_holder(struct spdk_scsi_lun *lun,
				  struct spdk_scsi_pr_registrant *reg)
{
	if (spdk_scsi_pr_is_all_registrants_type(lun)) {
		return true;
	}

	return (lun->reservation.holder == reg);
}

static int
spdk_scsi_pr_register_registrant(struct spdk_scsi_lun *lun,
				 struct spdk_scsi_port *initiator_port,
				 struct spdk_scsi_port *target_port,
				 uint64_t sa_rkey)
{
	struct spdk_scsi_pr_registrant *reg;

	/* Register sa_rkey with the I_T nexus */
	reg = calloc(1, sizeof(*reg));
	if (!reg) {
		return -ENOMEM;
	}

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REGISTER: new registrant registered "
		      "with key 0x%"PRIx64"\n", sa_rkey);

	/* New I_T nexus */
	reg->initiator_port = initiator_port;
	snprintf(reg->initiator_port_name, sizeof(reg->initiator_port_name), "%s",
		 initiator_port->name);
	reg->transport_id_len = initiator_port->transport_id_len;
	memcpy(reg->transport_id, initiator_port->transport_id, reg->transport_id_len);
	reg->target_port = target_port;
	snprintf(reg->target_port_name, sizeof(reg->target_port_name), "%s",
		 target_port->name);
	reg->relative_target_port_id = target_port->index;
	reg->rkey = sa_rkey;
	TAILQ_INSERT_TAIL(&lun->reg_head, reg, link);
	lun->pr_generation++;

	return 0;
}

static void
spdk_scsi_pr_release_reservation(struct spdk_scsi_lun *lun, struct spdk_scsi_pr_registrant *reg)
{
	bool all_regs = false;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REGISTER: release reservation "
		      "with type %u\n", lun->reservation.rtype);

	/* TODO: Unit Attention */
	all_regs = spdk_scsi_pr_is_all_registrants_type(lun);
	if (all_regs && !TAILQ_EMPTY(&lun->reg_head)) {
		lun->reservation.holder = TAILQ_FIRST(&lun->reg_head);
		return;
	}

	memset(&lun->reservation, 0, sizeof(struct spdk_scsi_pr_reservation));
}

static void
spdk_scsi_pr_reserve_reservation(struct spdk_scsi_lun *lun,
				 enum spdk_scsi_pr_type_code type,
				 uint64_t rkey,
				 struct spdk_scsi_pr_registrant *holder)
{
	lun->reservation.rtype = type;
	lun->reservation.crkey = rkey;
	lun->reservation.holder = holder;
}

static void
spdk_scsi_pr_unregister_registrant(struct spdk_scsi_lun *lun,
				   struct spdk_scsi_pr_registrant *reg)
{
	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REGISTER: unregister registrant\n");

	TAILQ_REMOVE(&lun->reg_head, reg, link);
	if (spdk_scsi_pr_registrant_is_holder(lun, reg)) {
		spdk_scsi_pr_release_reservation(lun, reg);
	}

	free(reg);
	lun->pr_generation++;
}

static void
spdk_scsi_pr_replace_registrant_key(struct spdk_scsi_lun *lun,
				    struct spdk_scsi_pr_registrant *reg,
				    uint64_t sa_rkey)
{
	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REGISTER: replace with new "
		      "reservation key 0x%"PRIx64"\n", sa_rkey);
	reg->rkey = sa_rkey;
	lun->pr_generation++;
}

static int
spdk_scsi_pr_out_reserve(struct spdk_scsi_task *task,
			 enum spdk_scsi_pr_type_code rtype, uint64_t rkey,
			 uint8_t spec_i_pt, uint8_t all_tg_pt, uint8_t aptpl)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_scsi_pr_registrant *reg;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR OUT RESERVE: rkey 0x%"PRIx64", requested "
		      "reservation type %u, type %u\n", rkey, rtype, lun->reservation.rtype);

	/* TODO: don't support now */
	if (spec_i_pt || all_tg_pt || aptpl) {
		SPDK_ERRLOG("Unspported spec_i_pt/all_tg_pt fields "
			    "or invalid aptpl field\n");
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
					  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -EINVAL;
	}

	reg = spdk_scsi_pr_get_registrant(lun, task->initiator_port, task->target_port);
	/* No registration for the I_T nexus */
	if (!reg) {
		SPDK_ERRLOG("No registration\n");
		goto conflict;
	}

	/* invalid reservation key */
	if (reg->rkey != rkey) {
		SPDK_ERRLOG("Reservation key 0x%"PRIx64" don't match 0x%"PRIx64"\n",
			    rkey, reg->rkey);
		goto conflict;
	}

	/* reservation holder already exists */
	if (lun->reservation.holder) {
		if (rtype != lun->reservation.rtype) {
			SPDK_ERRLOG("Reservation type doesn't match\n");
			goto conflict;
		}

		if (!spdk_scsi_pr_registrant_is_holder(lun, reg)) {
			SPDK_ERRLOG("Only 1 holder is allowed for type %u\n", rtype);
			goto conflict;
		}
	} else {
		/* current I_T nexus is the first reservation holder */
		spdk_scsi_pr_reserve_reservation(lun, rtype, rkey, reg);
	}

	return 0;

conflict:
	spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_RESERVATION_CONFLICT,
				  SPDK_SCSI_SENSE_NO_SENSE,
				  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
				  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
	return -EINVAL;
}

static int
spdk_scsi_pr_out_register(struct spdk_scsi_task *task,
			  enum spdk_scsi_pr_out_service_action_code action,
			  uint64_t rkey, uint64_t sa_rkey,
			  uint8_t spec_i_pt, uint8_t all_tg_pt, uint8_t aptpl)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_scsi_pr_registrant *reg;
	int sc, sk, asc;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR OUT REGISTER: rkey 0x%"PRIx64", "
		      "sa_key 0x%"PRIx64", reservation type %u\n", rkey, sa_rkey, lun->reservation.rtype);

	/* TODO: don't support now */
	if (spec_i_pt || all_tg_pt || aptpl) {
		SPDK_ERRLOG("Unsupported spec_i_pt/all_tg_pt/aptpl field\n");
		sc = SPDK_SCSI_STATUS_CHECK_CONDITION;
		sk = SPDK_SCSI_SENSE_ILLEGAL_REQUEST;
		asc = SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB;
		goto error_exit;
	}

	reg = spdk_scsi_pr_get_registrant(lun, task->initiator_port, task->target_port);
	/* an unregistered I_T nexus session */
	if (!reg) {
		if (rkey && (action == SPDK_SCSI_PR_OUT_REGISTER)) {
			SPDK_ERRLOG("Reservation key field is not empty\n");
			sc = SPDK_SCSI_STATUS_RESERVATION_CONFLICT;
			sk = SPDK_SCSI_SENSE_NO_SENSE;
			asc = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
			goto error_exit;
		}

		if (!sa_rkey) {
			/* Do nothing except return GOOD status */
			SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REGISTER: service action "
				      "reservation key is zero, do noting\n");
			return 0;
		}
		/* Add a new registrant for the I_T nexus */
		return spdk_scsi_pr_register_registrant(lun, task->initiator_port,
							task->target_port, sa_rkey);
	} else {
		/* a registered I_T nexus */
		if (rkey != reg->rkey && action == SPDK_SCSI_PR_OUT_REGISTER) {
			SPDK_ERRLOG("Reservation key 0x%"PRIx64" don't match "
				    "registrant's key 0x%"PRIx64"\n", rkey, reg->rkey);
			sc = SPDK_SCSI_STATUS_RESERVATION_CONFLICT;
			sk = SPDK_SCSI_SENSE_NO_SENSE;
			asc = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
			goto error_exit;
		}

		if (!sa_rkey) {
			/* unregister */
			spdk_scsi_pr_unregister_registrant(lun, reg);
		} else {
			/* replace */
			spdk_scsi_pr_replace_registrant_key(lun, reg, sa_rkey);
		}
	}

	return 0;

error_exit:
	spdk_scsi_task_set_status(task, sc, sk, asc, SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE);
	return -EINVAL;
}

static int
spdk_scsi_pr_out_release(struct spdk_scsi_task *task,
			 enum spdk_scsi_pr_type_code rtype, uint64_t rkey)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_scsi_pr_registrant *reg;
	int sk, asc;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR OUT RELEASE: rkey 0x%"PRIx64", "
		      "reservation type %u\n", rkey, rtype);

	reg = spdk_scsi_pr_get_registrant(lun, task->initiator_port, task->target_port);
	if (!reg) {
		SPDK_ERRLOG("No registration\n");
		sk = SPDK_SCSI_SENSE_NOT_READY;
		asc = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
		goto check_condition;
	}

	/* no reservation holder */
	if (!lun->reservation.holder) {
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "RELEASE: no reservation holder\n");
		return 0;
	}

	if (lun->reservation.rtype != rtype || rkey != lun->reservation.crkey) {
		sk = SPDK_SCSI_SENSE_ILLEGAL_REQUEST;
		asc = SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB;
		goto check_condition;
	}

	/* I_T nexus is not a persistent reservation holder */
	if (!spdk_scsi_pr_registrant_is_holder(lun, reg)) {
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "RELEASE: current I_T nexus is not holder\n");
		return 0;
	}

	spdk_scsi_pr_release_reservation(lun, reg);

	return 0;

check_condition:
	spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION, sk, asc,
				  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
	return -EINVAL;
}

static int
spdk_scsi_pr_out_clear(struct spdk_scsi_task *task, uint64_t rkey)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_scsi_pr_registrant *reg, *tmp;
	int sc, sk, asc;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR OUT CLEAR: rkey 0x%"PRIx64"\n", rkey);

	reg = spdk_scsi_pr_get_registrant(lun, task->initiator_port, task->target_port);
	if (!reg) {
		SPDK_ERRLOG("No registration\n");
		sc = SPDK_SCSI_STATUS_CHECK_CONDITION;
		sk = SPDK_SCSI_SENSE_NOT_READY;
		asc = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
		goto error_exit;
	}

	if (rkey != reg->rkey) {
		SPDK_ERRLOG("Reservation key 0x%"PRIx64" doesn't match "
			    "registrant's key 0x%"PRIx64"\n", rkey, reg->rkey);
		sc = SPDK_SCSI_STATUS_RESERVATION_CONFLICT;
		sk = SPDK_SCSI_SENSE_NO_SENSE;
		asc = SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE;
		goto error_exit;
	}

	TAILQ_FOREACH_SAFE(reg, &lun->reg_head, link, tmp) {
		spdk_scsi_pr_unregister_registrant(lun, reg);
	}

	return 0;

error_exit:
	spdk_scsi_task_set_status(task, sc, sk, asc, SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
	return -EINVAL;
}

int
spdk_scsi_pr_out(struct spdk_scsi_task *task,
		 uint8_t *cdb, uint8_t *data,
		 uint16_t data_len)
{
	int rc = -1;
	uint64_t rkey, sa_rkey;
	uint8_t spec_i_pt, all_tg_pt, aptpl;
	enum spdk_scsi_pr_out_service_action_code action;
	enum spdk_scsi_pr_scope_code scope;
	enum spdk_scsi_pr_type_code rtype;
	struct spdk_scsi_pr_out_param_list *param = (struct spdk_scsi_pr_out_param_list *)data;

	action = cdb[1] & 0x0f;
	scope = (cdb[2] >> 4) & 0x0f;
	rtype = cdb[2] & 0x0f;

	rkey = from_be64(&param->rkey);
	sa_rkey = from_be64(&param->sa_rkey);
	aptpl = param->aptpl;
	spec_i_pt = param->spec_i_pt;
	all_tg_pt = param->all_tg_pt;

	switch (action) {
	case SPDK_SCSI_PR_OUT_REGISTER:
	case SPDK_SCSI_PR_OUT_REG_AND_IGNORE_KEY:
		rc = spdk_scsi_pr_out_register(task, action, rkey, sa_rkey,
					       spec_i_pt, all_tg_pt, aptpl);
		break;
	case SPDK_SCSI_PR_OUT_RESERVE:
		if (scope != SPDK_SCSI_PR_LU_SCOPE) {
			goto invalid;
		}
		rc = spdk_scsi_pr_out_reserve(task, rtype, rkey,
					      spec_i_pt, all_tg_pt, aptpl);
		break;
	case SPDK_SCSI_PR_OUT_RELEASE:
		if (scope != SPDK_SCSI_PR_LU_SCOPE) {
			goto invalid;
		}
		rc = spdk_scsi_pr_out_release(task, rtype, rkey);
		break;
	case SPDK_SCSI_PR_OUT_CLEAR:
		rc = spdk_scsi_pr_out_clear(task, rkey);
		break;
	default:
		SPDK_ERRLOG("Invalid service action code %u\n", action);
		goto invalid;
	}

	return rc;

invalid:
	spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
				  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
				  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
				  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
	return -EINVAL;
}
