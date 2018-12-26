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
spdk_scsi_pr_get_registrant(struct spdk_scsi_dev *dev,
			    struct spdk_scsi_port *initiator_port,
			    struct spdk_scsi_port *target_port)
{
	struct spdk_scsi_pr_registrant *reg;

	TAILQ_FOREACH(reg, &dev->reg_head, link) {
		if ((initiator_port == reg->initiator_port) &&
		    (target_port == reg->target_port)) {
			return reg;
		}
	}

	return NULL;
}

/* Reservation type is all registrants or not */
static inline bool
spdk_scsi_pr_is_all_registrants_type(struct spdk_scsi_dev *dev)
{
	if (dev->holder) {
		return (dev->type == SPDK_SCSI_PR_WRITE_EXCLUSIVE_ALL_REGS ||
			dev->type == SPDK_SCSI_PR_EXCLUSIVE_ACCESS_ALL_REGS);
	}

	return false;
}

/* Registrant is reservation holder or not */
static inline bool
spdk_scsi_pr_registrant_is_holder(struct spdk_scsi_dev *dev,
				  struct spdk_scsi_pr_registrant *reg)
{
	if (reg == NULL) {
		return false;
	} else if (dev->holder) {
		return (dev->holder == reg ||
			dev->type == SPDK_SCSI_PR_WRITE_EXCLUSIVE_ALL_REGS ||
			dev->type == SPDK_SCSI_PR_EXCLUSIVE_ACCESS_ALL_REGS);
	}

	return false;
}

static int
spdk_scsi_pr_out_register(struct spdk_scsi_task *task,
			  enum spdk_scsi_pr_out_service_action_code action,
			  uint64_t rkey, uint64_t sa_rkey,
			  uint8_t spec_i_pt, uint8_t all_tg_pt, uint8_t aptpl)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_scsi_dev *dev = lun->dev;
	struct spdk_scsi_pr_registrant *reg;
	bool all_regs = false;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR OUT REGISTER: rkey 0x%"PRIx64", "
		      "sa_key 0x%"PRIx64", reservation type %u\n", rkey, sa_rkey, dev->type);

	/* TODO: don't support now */
	if (spec_i_pt || all_tg_pt || aptpl) {
		SPDK_ERRLOG("REGISTER: unsupported spec_i_pt/all_tg_pt/aptpl field\n");
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
					  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -EINVAL;
	}

	pthread_mutex_lock(&dev->reservation_lock);

	reg = spdk_scsi_pr_get_registrant(dev, task->initiator_port, task->target_port);
	/* an unregisted I_T nexus session */
	if (!reg) {
		if (rkey && (action == SPDK_SCSI_PR_OUT_REGISTER)) {
			pthread_mutex_unlock(&dev->reservation_lock);
			SPDK_ERRLOG("REGISTER: reservation key field is not empty\n");
			spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_RESERVATION_CONFLICT,
						  SPDK_SCSI_SENSE_NO_SENSE,
						  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
						  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
			return -EINVAL;
		}

		if (!sa_rkey) {
			pthread_mutex_unlock(&dev->reservation_lock);
			/* Do nothing except return GOOD status */
			SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REGISTER: service action "
				      "reservation key is zero, do noting\n");
			return 0;
		}

		/* Register sa_rkey with the I_T nexus */
		reg = calloc(1, sizeof(*reg));
		if (!reg) {
			pthread_mutex_unlock(&dev->reservation_lock);
			return -ENOMEM;
		}
		/* New I_T nexus */
		reg->initiator_port = task->initiator_port;
		snprintf(reg->initiator_port_name, sizeof(reg->initiator_port_name), "%s",
			 task->initiator_port->name);
		reg->transport_id_len = task->initiator_port->transport_id_len;
		memcpy(reg->transport_id, task->initiator_port->transport_id, reg->transport_id_len);
		reg->target_port = task->target_port;
		snprintf(reg->target_port_name, sizeof(reg->target_port_name), "%s",
			 task->target_port->name);
		reg->relative_target_port_id = task->target_port->index;
		reg->rkey = sa_rkey;
		TAILQ_INSERT_TAIL(&dev->reg_head, reg, link);

		dev->pr_generation++;
		pthread_mutex_unlock(&dev->reservation_lock);
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REGISTER: new registrant registered "
			      "with key 0x%"PRIx64"\n", sa_rkey);

		return 0;
	}

	/* a registred I_T nexus */
	if (rkey != reg->rkey && action == SPDK_SCSI_PR_OUT_REGISTER) {
		pthread_mutex_unlock(&dev->reservation_lock);
		SPDK_ERRLOG("REGISTER: reservation key 0x%"PRIx64" don't match "
			    "registrant's key 0x%"PRIx64"\n", rkey, reg->rkey);
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_RESERVATION_CONFLICT,
					  SPDK_SCSI_SENSE_NO_SENSE,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -EINVAL;
	}

	all_regs = spdk_scsi_pr_is_all_registrants_type(dev);

	if (!sa_rkey) {
		/* unregister */
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REGISTER: unregister registrant\n");
		TAILQ_REMOVE(&dev->reg_head, reg, link);
		if (all_regs) {
			if (TAILQ_EMPTY(&dev->reg_head)) {
				SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REGISTER: release reservation "
					      "with type %u\n", dev->type);
				dev->crkey = 0;
				dev->type = 0;
				dev->holder = NULL;
			} else {
				dev->holder = TAILQ_FIRST(&dev->reg_head);
			}
		} else if (spdk_scsi_pr_registrant_is_holder(dev, reg)) {

			SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REGISTER: release reservation "
				      "with type %u\n", dev->type);
			dev->crkey = 0;
			dev->type = 0;
			dev->holder = NULL;
		}
		free(reg);
	} else {
		/* replace */
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REGISTER: replace with new "
			      "reservation key 0x%"PRIx64"\n", sa_rkey);
		reg->rkey = sa_rkey;
	}

	dev->pr_generation++;
	pthread_mutex_unlock(&dev->reservation_lock);

	return 0;
}

static int
spdk_scsi_pr_out_reserve(struct spdk_scsi_task *task,
			 enum spdk_scsi_pr_type_code type, uint64_t rkey,
			 uint8_t spec_i_pt, uint8_t all_tg_pt, uint8_t aptpl)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_scsi_dev *dev = lun->dev;
	struct spdk_scsi_pr_registrant *reg;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR OUT RESERVE: rkey 0x%"PRIx64", requested "
		      "reservation type %u, type %u\n", rkey, type, dev->type);

	/* TODO: don't support now */
	if (spec_i_pt || all_tg_pt || aptpl) {
		SPDK_ERRLOG("RESERVE: unspported spec_i_pt/all_tg_pt fields "
			    "or invalid aptpl field\n");
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
					  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -EINVAL;
	}

	pthread_mutex_lock(&dev->reservation_lock);

	reg = spdk_scsi_pr_get_registrant(dev, task->initiator_port, task->target_port);
	/* No registration for the I_T nexus */
	if (!reg) {
		SPDK_ERRLOG("RESERVE: no registration\n");
		goto conflict;
	}

	/* invalid reservation key */
	if (reg->rkey != rkey) {
		SPDK_ERRLOG("RESERVE: reservation key 0x%"PRIx64" don't match 0x%"PRIx64"\n",
			    rkey, reg->rkey);
		goto conflict;
	}

	/* reservation holder already exists */
	if (dev->holder) {
		if (type != dev->type) {
			SPDK_ERRLOG("RESERVE: reservation type doesn't match\n");
			goto conflict;
		}

		if (!spdk_scsi_pr_registrant_is_holder(dev, reg)) {
			SPDK_ERRLOG("RESERVE: only 1 holder is allowed for type %u\n", type);
			goto conflict;
		}

		pthread_mutex_unlock(&dev->reservation_lock);
		return 0;
	}

	/* current I_T nexus is the first reservation holder */
	dev->type = type;
	dev->crkey = rkey;
	dev->holder = reg;

	pthread_mutex_unlock(&dev->reservation_lock);

	return 0;

conflict:
	pthread_mutex_unlock(&dev->reservation_lock);
	spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_RESERVATION_CONFLICT,
				  SPDK_SCSI_SENSE_NO_SENSE,
				  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
				  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
	return -EINVAL;
}

static int
spdk_scsi_pr_out_release(struct spdk_scsi_task *task,
			 enum spdk_scsi_pr_type_code type, uint64_t rkey)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_scsi_dev *dev = lun->dev;
	struct spdk_scsi_pr_registrant *reg;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR OUT RELEASE: rkey 0x%"PRIx64", "
		      "reservation type %u\n", rkey, type);

	pthread_mutex_lock(&dev->reservation_lock);

	reg = spdk_scsi_pr_get_registrant(dev, task->initiator_port, task->target_port);
	if (!reg) {
		pthread_mutex_unlock(&dev->reservation_lock);
		SPDK_ERRLOG("RELEASE: no registration\n");
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NOT_READY,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -EINVAL;
	}

	/* no reservation holder */
	if (!dev->holder) {
		pthread_mutex_unlock(&dev->reservation_lock);
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "RELEASE: no reservation holder\n");
		return 0;
	}

	if (dev->type != type || rkey != dev->crkey) {
		SPDK_ERRLOG("RELEASE: type or reservation key doesn't match\n");
		goto check_condition;
	}

	/* I_T nexus is not a persistent reservation holder */
	if (!spdk_scsi_pr_registrant_is_holder(dev, reg)) {
		pthread_mutex_unlock(&dev->reservation_lock);
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "RELEASE: current I_T nexus is not holder\n");
		return 0;
	}

	/* TODO: Unit Attention to other initiator ports for
	 * type registrants only or all registrants
	 */
	dev->type = 0;
	dev->crkey = 0;
	dev->holder = NULL;
	pthread_mutex_unlock(&dev->reservation_lock);

	return 0;

check_condition:
	pthread_mutex_unlock(&dev->reservation_lock);
	spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
				  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
				  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
				  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
	return -EINVAL;
}

static int
spdk_scsi_pr_out_clear(struct spdk_scsi_task *task, uint64_t rkey)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_scsi_dev *dev = lun->dev;
	struct spdk_scsi_pr_registrant *reg, *tmp;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR OUT CLEAR: rkey 0x%"PRIx64"\n", rkey);

	pthread_mutex_lock(&dev->reservation_lock);
	reg = spdk_scsi_pr_get_registrant(dev, task->initiator_port, task->target_port);
	if (!reg) {
		pthread_mutex_unlock(&dev->reservation_lock);
		SPDK_ERRLOG("CLEAR: no registration\n");
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NOT_READY,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -EINVAL;
	}

	if (rkey != reg->rkey) {
		pthread_mutex_unlock(&dev->reservation_lock);
		SPDK_ERRLOG("CLEAR: reservation key 0x%"PRIx64" doesn't match "
			    "registrant's key 0x%"PRIx64"\n", rkey, reg->rkey);
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_RESERVATION_CONFLICT,
					  SPDK_SCSI_SENSE_NO_SENSE,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -EINVAL;
	}

	TAILQ_FOREACH(tmp, &dev->reg_head, link) {
		TAILQ_REMOVE(&dev->reg_head, tmp, link);
		if (tmp != reg) {
			/* TODO: Unit Attention for other initiator ports */
		}
		free(tmp);
	}

	if (dev->holder) {
		dev->crkey = 0;
		dev->type = 0;
		dev->holder = NULL;
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "CLEAR: release reservation\n");
	}

	dev->pr_generation++;
	pthread_mutex_unlock(&dev->reservation_lock);

	return 0;
}

static void
spdk_scsi_pr_remove_all_regs_by_key(struct spdk_scsi_dev *dev, uint64_t sa_rkey)
{
	struct spdk_scsi_pr_registrant *reg;

	TAILQ_FOREACH(reg, &dev->reg_head, link) {
		if (reg->rkey == sa_rkey) {
			TAILQ_REMOVE(&dev->reg_head, reg, link);
			free(reg);
		}
	}
}

static int
spdk_scsi_pr_out_preempt(struct spdk_scsi_task *task,
			 enum spdk_scsi_pr_out_service_action_code action,
			 enum spdk_scsi_pr_type_code type,
			 uint64_t rkey, uint64_t sa_rkey)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_scsi_dev *dev = lun->dev;
	struct spdk_scsi_pr_registrant *reg, *tmp;
	bool all_regs = false;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR OUT PREEMPT: rkey 0x%"PRIx64", sa_rkey 0x%"PRIx64" "
		      "action %u, type %u, reservation type %u\n",
		      rkey, sa_rkey, action, type, dev->type);

	pthread_mutex_lock(&dev->reservation_lock);

	/* I_T nexus is not registered */
	reg = spdk_scsi_pr_get_registrant(dev, task->initiator_port, task->target_port);
	if (!reg) {
		SPDK_ERRLOG("PREEMPT: no registration\n");
		goto conflict;
	}
	if (rkey != reg->rkey) {
		SPDK_ERRLOG("PREEMPT: reservation key 0x%"PRIx64" doesn't match "
			    "registrant's key 0x%"PRIx64"\n", rkey, reg->rkey);
		goto conflict;
	}

	/* no persistent reservation */
	if (dev->holder == NULL) {
		spdk_scsi_pr_remove_all_regs_by_key(dev, sa_rkey);
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PREEMPT: no persistent reservation\n");
		goto exit;
	}

	all_regs = spdk_scsi_pr_is_all_registrants_type(dev);

	if (all_regs && sa_rkey != 0) {
		spdk_scsi_pr_remove_all_regs_by_key(dev, sa_rkey);
		if (TAILQ_EMPTY(&dev->reg_head)) {
			dev->type = 0;
			dev->crkey = 0;
			dev->holder = NULL;
		} else {
			dev->holder = TAILQ_FIRST(&dev->reg_head);
		}
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PREEMPT: All registrants type with sa_rkey\n");
		goto exit;
	}

	if (all_regs && sa_rkey == 0) {
		/* 1. remove all other registrants
		 * 2. release persistent reservation
		 * 3. create persistent reservation using new type and scope */
		TAILQ_FOREACH(tmp, &dev->reg_head, link) {
			if (tmp == reg) {
				continue;
			}
			TAILQ_REMOVE(&dev->reg_head, tmp, link);
			free(tmp);
		}
		dev->type = type;
		dev->crkey = rkey;
		dev->holder = reg;
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PREEMPT: All registrants type with sa_rkey zeroed\n");
		goto exit;
	}

	assert(all_regs == false);

	/* preempt itself for non all registrants type */
	if (spdk_scsi_pr_registrant_is_holder(dev, reg) && sa_rkey == dev->crkey) {
		dev->type = type;
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PREEMPT: preempt itself with type %u\n", type);
		goto exit;
	}

	if (!sa_rkey) {
		pthread_mutex_unlock(&dev->reservation_lock);
		SPDK_ERRLOG("PREEMPT: zeroed sa_rkey\n");
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
					  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -EINVAL;
	}

	spdk_scsi_pr_remove_all_regs_by_key(dev, sa_rkey);
	reg = spdk_scsi_pr_get_registrant(dev, task->initiator_port, task->target_port);
	if (!reg) {
		SPDK_ERRLOG("PREEMPT: current I_T nexus registrant was removed\n");
		goto conflict;
	}

	/* preempt other holder */
	if (sa_rkey == dev->crkey) {
		dev->type = type;
		dev->crkey = rkey;
		dev->holder = reg;
	}

exit:
	dev->pr_generation++;
	pthread_mutex_unlock(&dev->reservation_lock);
	return 0;

conflict:
	pthread_mutex_unlock(&dev->reservation_lock);
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
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_dev *dev;
	int rc = -1;
	uint64_t rkey, sa_rkey;
	uint8_t spec_i_pt, all_tg_pt, aptpl;
	enum spdk_scsi_pr_out_service_action_code action;
	enum spdk_scsi_pr_scope_code scope;
	enum spdk_scsi_pr_type_code type;
	struct spdk_scsi_pr_out_param_list *param = (struct spdk_scsi_pr_out_param_list *)data;

	lun = task->lun;
	if (!lun) {
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NOT_READY,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -EINVAL;
	}
	dev = lun->dev;
	if (!dev) {
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NOT_READY,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -EINVAL;
	}

	action = cdb[1] & 0x0f;
	scope = (cdb[2] >> 4) & 0x0f;
	type = cdb[2] & 0x0f;

	if (scope != SPDK_SCSI_PR_LU_SCOPE) {
		goto invalid;
	}

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
		rc = spdk_scsi_pr_out_reserve(task, type, rkey,
					      spec_i_pt, all_tg_pt, aptpl);
		break;
	case SPDK_SCSI_PR_OUT_RELEASE:
		rc = spdk_scsi_pr_out_release(task, type, rkey);
		break;
	case SPDK_SCSI_PR_OUT_CLEAR:
		rc = spdk_scsi_pr_out_clear(task, rkey);
		break;
	case SPDK_SCSI_PR_OUT_PREEMPT:
		rc = spdk_scsi_pr_out_preempt(task, action, type, rkey, sa_rkey);
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
	struct spdk_scsi_dev *dev = lun->dev;
	struct spdk_scsi_pr_registrant *reg;
	struct spdk_scsi_pr_in_read_keys_data *keys;
	uint16_t count = 0;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR IN READ KEYS\n");

	if (data_len < sizeof(keys->header)) {
		return -ENOMEM;
	}

	pthread_mutex_lock(&dev->reservation_lock);
	keys = (struct spdk_scsi_pr_in_read_keys_data *)data;

	to_be32(&keys->header.pr_generation, dev->pr_generation);
	TAILQ_FOREACH(reg, &dev->reg_head, link) {
		if ((count * 8 + sizeof(keys->header)) > data_len) {
			break;
		}
		to_be64(&keys->rkeys[count], reg->rkey);
		count++;
	}
	to_be32(&keys->header.addiontal_len, count * 8);

	pthread_mutex_unlock(&dev->reservation_lock);
	return (sizeof(keys->header) + count * 8);
}

int
spdk_scsi_pr_in(struct spdk_scsi_task *task,
		uint8_t *cdb, uint8_t *data,
		uint16_t data_len)
{
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_dev *dev;
	enum spdk_scsi_pr_in_action_code action;
	int rc = 0;

	lun = task->lun;
	if (!lun) {
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NOT_READY,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -EINVAL;
	}
	dev = lun->dev;
	if (!dev) {
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NOT_READY,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -EINVAL;
	}

	action = cdb[1] & 0x1f;

	switch (action) {
	case SPDK_SCSI_PR_IN_READ_KEYS:
		rc = spdk_scsi_pr_in_read_keys(task, data, data_len);
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
