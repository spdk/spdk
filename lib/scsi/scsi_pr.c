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

/* LUN holds a reservation or not */
static inline bool
spdk_scsi_pr_has_reservation(struct spdk_scsi_lun *lun)
{
	return !(lun->reservation.holder == NULL);
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
	if (initiator_port) {
		snprintf(reg->initiator_port_name, sizeof(reg->initiator_port_name), "%s",
			 initiator_port->name);
		reg->transport_id_len = initiator_port->transport_id_len;
		memcpy(reg->transport_id, initiator_port->transport_id, reg->transport_id_len);
	}
	reg->target_port = target_port;
	if (target_port) {
		snprintf(reg->target_port_name, sizeof(reg->target_port_name), "%s",
			 target_port->name);
		reg->relative_target_port_id = target_port->index;
	}
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
	if (spdk_scsi_pr_has_reservation(lun)) {
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
	if (!spdk_scsi_pr_has_reservation(lun)) {
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

static void
spdk_scsi_pr_remove_all_regs_by_key(struct spdk_scsi_lun *lun, uint64_t sa_rkey)
{
	struct spdk_scsi_pr_registrant *reg, *tmp;

	TAILQ_FOREACH_SAFE(reg, &lun->reg_head, link, tmp) {
		if (reg->rkey == sa_rkey) {
			spdk_scsi_pr_unregister_registrant(lun, reg);
		}
	}
}

static void
spdk_scsi_pr_remove_all_other_regs(struct spdk_scsi_lun *lun, struct spdk_scsi_pr_registrant *reg)
{
	struct spdk_scsi_pr_registrant *reg_tmp, *reg_tmp2;

	TAILQ_FOREACH_SAFE(reg_tmp, &lun->reg_head, link, reg_tmp2) {
		if (reg_tmp != reg) {
			spdk_scsi_pr_unregister_registrant(lun, reg_tmp);
		}
	}
}

static int
spdk_scsi_pr_out_preempt(struct spdk_scsi_task *task,
			 enum spdk_scsi_pr_out_service_action_code action,
			 enum spdk_scsi_pr_type_code rtype,
			 uint64_t rkey, uint64_t sa_rkey)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_scsi_pr_registrant *reg;
	bool all_regs = false;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR OUT PREEMPT: rkey 0x%"PRIx64", sa_rkey 0x%"PRIx64" "
		      "action %u, type %u, reservation type %u\n",
		      rkey, sa_rkey, action, rtype, lun->reservation.rtype);

	/* I_T nexus is not registered */
	reg = spdk_scsi_pr_get_registrant(lun, task->initiator_port, task->target_port);
	if (!reg) {
		SPDK_ERRLOG("No registration\n");
		goto conflict;
	}
	if (rkey != reg->rkey) {
		SPDK_ERRLOG("Reservation key 0x%"PRIx64" doesn't match "
			    "registrant's key 0x%"PRIx64"\n", rkey, reg->rkey);
		goto conflict;
	}

	/* no persistent reservation */
	if (!spdk_scsi_pr_has_reservation(lun)) {
		spdk_scsi_pr_remove_all_regs_by_key(lun, sa_rkey);
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PREEMPT: no persistent reservation\n");
		goto exit;
	}

	all_regs = spdk_scsi_pr_is_all_registrants_type(lun);

	if (all_regs) {
		if (sa_rkey != 0) {
			spdk_scsi_pr_remove_all_regs_by_key(lun, sa_rkey);
			SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PREEMPT: All registrants type with sa_rkey\n");
		} else {
			/* remove all other registrants and release persistent reservation if any */
			spdk_scsi_pr_remove_all_other_regs(lun, reg);
			/* create persistent reservation using new type and scope */
			spdk_scsi_pr_reserve_reservation(lun, rtype, 0, reg);
			SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PREEMPT: All registrants type with sa_rkey zeroed\n");
		}
		goto exit;
	}

	assert(lun->reservation.crkey != 0);

	if (sa_rkey != lun->reservation.crkey) {
		if (!sa_rkey) {
			SPDK_ERRLOG("Zeroed sa_rkey\n");
			spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
						  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
						  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
						  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
			return -EINVAL;
		}
		spdk_scsi_pr_remove_all_regs_by_key(lun, sa_rkey);
		goto exit;
	}

	if (spdk_scsi_pr_registrant_is_holder(lun, reg)) {
		spdk_scsi_pr_reserve_reservation(lun, rtype, rkey, reg);
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PREEMPT: preempt itself with type %u\n", rtype);
		goto exit;
	}

	/* unregister registrants if any */
	spdk_scsi_pr_remove_all_regs_by_key(lun, sa_rkey);
	reg = spdk_scsi_pr_get_registrant(lun, task->initiator_port, task->target_port);
	if (!reg) {
		SPDK_ERRLOG("Current I_T nexus registrant was removed\n");
		goto conflict;
	}

	/* preempt the holder */
	spdk_scsi_pr_reserve_reservation(lun, rtype, rkey, reg);

exit:
	lun->pr_generation++;
	return 0;

conflict:
	spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_RESERVATION_CONFLICT,
				  SPDK_SCSI_SENSE_NO_SENSE,
				  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
				  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
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
	case SPDK_SCSI_PR_OUT_PREEMPT:
		if (scope != SPDK_SCSI_PR_LU_SCOPE) {
			goto invalid;
		}
		rc = spdk_scsi_pr_out_preempt(task, action, rtype, rkey, sa_rkey);
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

static int
spdk_scsi_pr_in_read_keys(struct spdk_scsi_task *task, uint8_t *data,
			  uint16_t data_len)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_scsi_pr_in_read_keys_data *keys;
	struct spdk_scsi_pr_registrant *reg, *tmp;
	uint16_t count = 0;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR IN READ KEYS\n");
	keys = (struct spdk_scsi_pr_in_read_keys_data *)data;

	to_be32(&keys->header.pr_generation, lun->pr_generation);
	TAILQ_FOREACH_SAFE(reg, &lun->reg_head, link, tmp) {
		if (((count + 1) * 8 + sizeof(keys->header)) > data_len) {
			break;
		}
		to_be64(&keys->rkeys[count], reg->rkey);
		count++;
	}
	to_be32(&keys->header.additional_len, count * 8);

	return (sizeof(keys->header) + count * 8);
}

static int
spdk_scsi_pr_in_read_reservations(struct spdk_scsi_task *task,
				  uint8_t *data, uint16_t data_len)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_scsi_pr_in_read_reservations_data *param;
	bool all_regs = false;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR IN READ RESERVATIONS\n");
	param = (struct spdk_scsi_pr_in_read_reservations_data *)(data);

	to_be32(&param->header.pr_generation, lun->pr_generation);
	if (spdk_scsi_pr_has_reservation(lun)) {
		all_regs = spdk_scsi_pr_is_all_registrants_type(lun);
		if (all_regs) {
			to_be64(&param->rkey, 0);
		} else {
			to_be64(&param->rkey, lun->reservation.crkey);
		}
		to_be32(&param->header.additional_len, 16);
		param->scope = SPDK_SCSI_PR_LU_SCOPE;
		param->type = lun->reservation.rtype;
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "READ RESERVATIONS with valid reservation\n");
		return sizeof(*param);
	}

	/* no reservation */
	to_be32(&param->header.additional_len, 0);
	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "READ RESERVATIONS no reservation\n");
	return sizeof(param->header);
}

static int
spdk_scsi_pr_in_report_capabilities(struct spdk_scsi_task *task,
				    uint8_t *data, uint16_t data_len)
{
	struct spdk_scsi_pr_in_report_capabilities_data *param;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR IN REPORT CAPABILITIES\n");
	param = (struct spdk_scsi_pr_in_report_capabilities_data *)data;

	memset(param, 0, sizeof(*param));
	/* TODO: can support more capabilities bits */
	to_be16(&param->length, sizeof(*param));
	param->tmv = 1;
	param->wr_ex = 1;
	param->ex_ac = 1;
	param->wr_ex_ro = 1;
	param->ex_ac_ro = 1;
	param->wr_ex_ar = 1;
	param->ex_ac_ar = 1;

	return sizeof(*param);
}

static int
spdk_scsi_pr_in_read_full_status(struct spdk_scsi_task *task,
				 uint8_t *data, uint16_t data_len)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_scsi_pr_in_full_status_data *param;
	struct spdk_scsi_pr_in_full_status_desc *desc;
	struct spdk_scsi_pr_registrant *reg, *tmp;
	bool all_regs = false;
	uint32_t add_len = 0;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR IN READ FULL STATUS\n");

	all_regs = spdk_scsi_pr_is_all_registrants_type(lun);
	param = (struct spdk_scsi_pr_in_full_status_data *)data;
	to_be32(&param->header.pr_generation, lun->pr_generation);

	TAILQ_FOREACH_SAFE(reg, &lun->reg_head, link, tmp) {
		desc = (struct spdk_scsi_pr_in_full_status_desc *)
		       ((uint8_t *)param->desc_list + add_len);
		if (add_len + sizeof(*desc) + sizeof(param->header) > data_len) {
			break;
		}
		add_len += sizeof(*desc);
		desc->rkey = reg->rkey;
		if (all_regs || lun->reservation.holder == reg) {
			desc->r_holder = true;
			desc->type = lun->reservation.rtype;
		} else {
			desc->r_holder = false;
			desc->type = 0;
		}
		desc->all_tg_pt = 0;
		desc->scope = SPDK_SCSI_PR_LU_SCOPE;
		desc->relative_target_port_id = reg->relative_target_port_id;
		if (add_len + reg->transport_id_len + sizeof(param->header) > data_len) {
			break;
		}
		add_len += reg->transport_id_len;
		memcpy(&desc->transport_id, reg->transport_id, reg->transport_id_len);
		to_be32(&desc->desc_len, reg->transport_id_len);
	}
	to_be32(&param->header.additional_len, add_len);

	return (sizeof(param->header) + add_len);
}

int
spdk_scsi_pr_in(struct spdk_scsi_task *task,
		uint8_t *cdb, uint8_t *data,
		uint16_t data_len)
{
	enum spdk_scsi_pr_in_action_code action;
	int rc = 0;

	action = cdb[1] & 0x1f;
	if (data_len < sizeof(struct spdk_scsi_pr_in_read_header)) {
		goto invalid;
	}

	switch (action) {
	case SPDK_SCSI_PR_IN_READ_KEYS:
		rc = spdk_scsi_pr_in_read_keys(task, data, data_len);
		break;
	case SPDK_SCSI_PR_IN_READ_RESERVATION:
		if (data_len < sizeof(struct spdk_scsi_pr_in_read_reservations_data)) {
			goto invalid;
		}
		rc = spdk_scsi_pr_in_read_reservations(task, data, data_len);
		break;
	case SPDK_SCSI_PR_IN_REPORT_CAPABILITIES:
		rc = spdk_scsi_pr_in_report_capabilities(task, data, data_len);
		break;
	case SPDK_SCSI_PR_IN_READ_FULL_STATUS:
		rc = spdk_scsi_pr_in_read_full_status(task, data, data_len);
		break;
	default:
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

int
spdk_scsi_pr_check(struct spdk_scsi_task *task)
{
	struct spdk_scsi_lun *lun = task->lun;
	uint8_t *cdb = task->cdb;
	enum spdk_scsi_pr_type_code rtype;
	enum spdk_scsi_pr_out_service_action_code action;
	struct spdk_scsi_pr_registrant *reg;
	bool dma_to_device = false;

	/* no reservation holders */
	if (!spdk_scsi_pr_has_reservation(lun)) {
		return 0;
	}

	rtype = lun->reservation.rtype;
	assert(rtype != 0);

	reg = spdk_scsi_pr_get_registrant(lun, task->initiator_port, task->target_port);
	/* current I_T nexus hold the reservation */
	if (spdk_scsi_pr_registrant_is_holder(lun, reg)) {
		return 0;
	}

	/* reservation is held by other I_T nexus */
	switch (cdb[0]) {
	case SPDK_SPC_INQUIRY:
	case SPDK_SPC_REPORT_LUNS:
	case SPDK_SPC_REQUEST_SENSE:
	case SPDK_SPC_LOG_SENSE:
	case SPDK_SPC_TEST_UNIT_READY:
	case SPDK_SBC_START_STOP_UNIT:
	case SPDK_SBC_READ_CAPACITY_10:
	case SPDK_SPC_PERSISTENT_RESERVE_IN:
	case SPDK_SPC_SERVICE_ACTION_IN_16:
		return 0;
	case SPDK_SPC_MODE_SELECT_6:
	case SPDK_SPC_MODE_SELECT_10:
	case SPDK_SPC_MODE_SENSE_6:
	case SPDK_SPC_MODE_SENSE_10:
	case SPDK_SPC_LOG_SELECT:
		/* I_T nexus is registrant but not holder */
		if (!reg) {
			SPDK_DEBUGLOG(SPDK_LOG_SCSI, "CHECK: current I_T nexus "
				      "is not registered, cdb 0x%x\n", cdb[0]);
			goto conflict;
		}
		break;
	case SPDK_SPC_PERSISTENT_RESERVE_OUT:
		action = cdb[1] & 0x1f;
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "CHECK: PR OUT action %u\n", action);
		switch (action) {
		case SPDK_SCSI_PR_OUT_RELEASE:
		case SPDK_SCSI_PR_OUT_CLEAR:
		case SPDK_SCSI_PR_OUT_PREEMPT:
		case SPDK_SCSI_PR_OUT_PREEMPT_AND_ABORT:
			if (!reg) {
				SPDK_ERRLOG("CHECK: PR OUT action %u\n", action);
				goto conflict;
			}
			break;
		case SPDK_SCSI_PR_OUT_REGISTER:
		case SPDK_SCSI_PR_OUT_REG_AND_IGNORE_KEY:
			return 0;
		case SPDK_SCSI_PR_OUT_REG_AND_MOVE:
			SPDK_ERRLOG("CHECK: PR OUT action %u\n", action);
			goto conflict;
		default:
			SPDK_ERRLOG("CHECK: PR OUT invalid action %u\n", action);
			goto conflict;
		}

	/* For most SBC R/W commands */
	default:
		break;
	}

	switch (cdb[0]) {
	case SPDK_SBC_READ_6:
	case SPDK_SBC_READ_10:
	case SPDK_SBC_READ_12:
	case SPDK_SBC_READ_16:
		break;
	case SPDK_SBC_WRITE_6:
	case SPDK_SBC_WRITE_10:
	case SPDK_SBC_WRITE_12:
	case SPDK_SBC_WRITE_16:
	case SPDK_SBC_UNMAP:
	case SPDK_SBC_SYNCHRONIZE_CACHE_10:
	case SPDK_SBC_SYNCHRONIZE_CACHE_16:
		dma_to_device = true;
		break;
	default:
		SPDK_ERRLOG("CHECK: unsupported SCSI command cdb 0x%x\n", cdb[0]);
		goto conflict;
	}

	switch (rtype) {
	case SPDK_SCSI_PR_WRITE_EXCLUSIVE:
		if (dma_to_device) {
			SPDK_ERRLOG("CHECK: Write Exclusive reservation type "
				    "rejects command 0x%x\n", cdb[0]);
			goto conflict;
		}
		break;
	case SPDK_SCSI_PR_EXCLUSIVE_ACCESS:
		SPDK_ERRLOG("CHECK: Exclusive Access reservation type "
			    "rejects command 0x%x\n", cdb[0]);
		goto conflict;
	case SPDK_SCSI_PR_WRITE_EXCLUSIVE_REGS_ONLY:
	case SPDK_SCSI_PR_WRITE_EXCLUSIVE_ALL_REGS:
		if (!reg && dma_to_device) {
			SPDK_ERRLOG("CHECK: Registrants only reservation "
				    "type  reject command 0x%x\n", cdb[0]);
			goto conflict;
		}
		break;
	case SPDK_SCSI_PR_EXCLUSIVE_ACCESS_REGS_ONLY:
	case SPDK_SCSI_PR_EXCLUSIVE_ACCESS_ALL_REGS:
		if (!reg) {
			SPDK_ERRLOG("CHECK: All Registrants reservation "
				    "type  reject command 0x%x\n", cdb[0]);
			goto conflict;
		}
		break;
	default:
		break;
	}

	return 0;

conflict:
	spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_RESERVATION_CONFLICT,
				  SPDK_SCSI_SENSE_NO_SENSE,
				  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
				  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
	return -1;
}
