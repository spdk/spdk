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
#include "spdk/util.h"
#include "spdk/json.h"

static int
spdk_scsi_dev_update_aptpl(struct spdk_scsi_dev *dev,
			   struct spdk_scsi_task *task, bool aptpl);

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

	/* Iterate the second time with port name for
	 * those registrants with valid port name but not
	 * associated port pointers.
	 */
	TAILQ_FOREACH(reg, &dev->reg_head, link) {
		if (!strncasecmp(reg->initiator_port_name, initiator_port->name,
				 strlen(reg->initiator_port_name)) &&
		    !strncasecmp(reg->target_port_name, target_port->name,
				 strlen(reg->target_port_name))) {
			assert(reg->initiator_port == NULL);
			assert(reg->target_port == NULL);
			reg->initiator_port = initiator_port;
			reg->target_port = target_port;
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
spdk_scsi_pr_register_registrant(struct spdk_scsi_dev *dev,
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
	TAILQ_INSERT_TAIL(&dev->reg_head, reg, link);
	dev->pr_generation++;

	return 0;
}

static void
spdk_scsi_pr_release_reservation(struct spdk_scsi_dev *dev, struct spdk_scsi_pr_registrant *reg)
{
	bool all_regs = false;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REGISTER: release reservation "
		      "with type %u\n", dev->type);

	/* TODO: Unit Attention */

	all_regs = spdk_scsi_pr_is_all_registrants_type(dev);
	if (all_regs) {
		if (TAILQ_EMPTY(&dev->reg_head)) {
			dev->crkey = 0;
			dev->type = 0;
			dev->holder = NULL;
		} else {
			dev->holder = TAILQ_FIRST(&dev->reg_head);
		}
		return;
	}

	dev->crkey = 0;
	dev->type = 0;
	dev->holder = NULL;
}

static void
spdk_scsi_pr_reserve_reservation(struct spdk_scsi_dev *dev,
				 enum spdk_scsi_pr_type_code type,
				 uint64_t rkey,
				 struct spdk_scsi_pr_registrant *holder)
{
	dev->type = type;
	dev->crkey = rkey;
	dev->holder = holder;
}

static void
spdk_scsi_pr_unregister_registrant(struct spdk_scsi_dev *dev,
				   struct spdk_scsi_pr_registrant *reg)
{

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REGISTER: unregister registrant\n");

	TAILQ_REMOVE(&dev->reg_head, reg, link);
	if (spdk_scsi_pr_registrant_is_holder(dev, reg)) {
		spdk_scsi_pr_release_reservation(dev, reg);
	}
	free(reg);
	dev->pr_generation++;
}

static void
spdk_scsi_pr_replace_registrant_key(struct spdk_scsi_dev *dev,
				    struct spdk_scsi_pr_registrant *reg,
				    uint64_t sa_rkey)
{

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REGISTER: replace with new "
		      "reservation key 0x%"PRIx64"\n", sa_rkey);
	reg->rkey = sa_rkey;
	dev->pr_generation++;
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
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR OUT REGISTER: rkey 0x%"PRIx64", "
		      "sa_key 0x%"PRIx64", reservation type %u\n", rkey, sa_rkey, dev->type);

	/* TODO: don't support now */
	if (spec_i_pt || all_tg_pt || (dev->pr_file == NULL && aptpl)) {
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
		/* Add a new registrant for the I_T nexus */
		rc = spdk_scsi_pr_register_registrant(dev, task->initiator_port,
						      task->target_port, sa_rkey);
		pthread_mutex_unlock(&dev->reservation_lock);
		if (rc != 0) {
			return rc;
		}

		return spdk_scsi_dev_update_aptpl(dev, task, aptpl);
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

	if (!sa_rkey) {
		/* unregister */
		spdk_scsi_pr_unregister_registrant(dev, reg);
	} else {
		/* replace */
		spdk_scsi_pr_replace_registrant_key(dev, reg, sa_rkey);
	}

	pthread_mutex_unlock(&dev->reservation_lock);
	return spdk_scsi_dev_update_aptpl(dev, task, aptpl);
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
	spdk_scsi_pr_reserve_reservation(dev, type, rkey, reg);

	pthread_mutex_unlock(&dev->reservation_lock);

	if (dev->ptpl_activated) {
		return spdk_scsi_dev_update_aptpl(dev, task, true);
	}
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

	spdk_scsi_pr_release_reservation(dev, reg);
	pthread_mutex_unlock(&dev->reservation_lock);

	if (dev->ptpl_activated) {
		return spdk_scsi_dev_update_aptpl(dev, task, true);
	}
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
		spdk_scsi_pr_unregister_registrant(dev, tmp);
	}

	pthread_mutex_unlock(&dev->reservation_lock);

	if (dev->ptpl_activated) {
		return spdk_scsi_dev_update_aptpl(dev, task, false);
	}
	return 0;
}

static void
spdk_scsi_pr_remove_all_regs_by_key(struct spdk_scsi_dev *dev, uint64_t sa_rkey)
{
	struct spdk_scsi_pr_registrant *reg;

	TAILQ_FOREACH(reg, &dev->reg_head, link) {
		if (reg->rkey == sa_rkey) {
			spdk_scsi_pr_unregister_registrant(dev, reg);
		}
	}
}

static void
spdk_scsi_pr_remove_all_other_regs(struct spdk_scsi_dev *dev, struct spdk_scsi_pr_registrant *reg)
{
	struct spdk_scsi_pr_registrant *tmp;

	TAILQ_FOREACH(tmp, &dev->reg_head, link) {
		if (tmp != reg) {
			spdk_scsi_pr_unregister_registrant(dev, tmp);
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
	struct spdk_scsi_pr_registrant *reg;
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
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PREEMPT: All registrants type with sa_rkey\n");
		goto exit;
	}

	if (all_regs && sa_rkey == 0) {
		/* 1. remove all other registrants
		 * 2. release persistent reservation
		 * 3. create persistent reservation using new type and scope */
		spdk_scsi_pr_remove_all_other_regs(dev, reg);
		spdk_scsi_pr_reserve_reservation(dev, type, 0, reg);
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PREEMPT: All registrants type with sa_rkey zeroed\n");
		goto exit;
	}

	assert(all_regs == false);
	assert(dev->crkey != 0);

	if (sa_rkey != dev->crkey) {
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
		goto exit;
	}

	if (spdk_scsi_pr_registrant_is_holder(dev, reg)) {
		spdk_scsi_pr_reserve_reservation(dev, type, rkey, reg);
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PREEMPT: preempt itself with type %u\n", type);
		goto exit;
	}

	/* unregister registrants if any */
	spdk_scsi_pr_remove_all_regs_by_key(dev, sa_rkey);
	reg = spdk_scsi_pr_get_registrant(dev, task->initiator_port, task->target_port);
	if (!reg) {
		SPDK_ERRLOG("PREEMPT: current I_T nexus registrant was removed\n");
		goto conflict;
	}

	/* preempt the holder */
	spdk_scsi_pr_reserve_reservation(dev, type, rkey, reg);

exit:
	dev->pr_generation++;
	pthread_mutex_unlock(&dev->reservation_lock);

	if (dev->ptpl_activated) {
		return spdk_scsi_dev_update_aptpl(dev, task, true);
	}
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
spdk_scsi_pr_in_read_full_status(struct spdk_scsi_task *task,
				 uint8_t *buf, uint16_t data_len)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_scsi_dev *dev = lun->dev;
	struct spdk_scsi_pr_in_full_status_data *data;
	struct spdk_scsi_pr_in_full_status_desc *desc;
	struct spdk_scsi_pr_registrant *reg;
	bool all_regs = false;
	uint32_t add_len = 0;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR IN READ FULL STATUS\n");

	if (data_len < sizeof(data->header)) {
		return -ENOMEM;
	}

	pthread_mutex_lock(&dev->reservation_lock);

	all_regs = spdk_scsi_pr_is_all_registrants_type(dev);

	data = (struct spdk_scsi_pr_in_full_status_data *)buf;

	to_be32(&data->header.pr_generation, dev->pr_generation);

	TAILQ_FOREACH(reg, &dev->reg_head, link) {
		desc = (struct spdk_scsi_pr_in_full_status_desc *)
		       ((uint8_t *)data->desc_list + add_len);
		add_len += sizeof(*desc);
		if (add_len + sizeof(data->header) > data_len) {
			break;
		}
		desc->rkey = reg->rkey;
		if (all_regs) {
			desc->r_holder = true;
			desc->type = dev->type;
		} else {
			desc->r_holder = (dev->holder == reg) ? true : false;
			desc->type = (dev->holder == reg) ? dev->type : 0;
		}
		desc->all_tg_pt = 0;
		desc->scope = SPDK_SCSI_PR_LU_SCOPE;
		desc->relative_target_port_id = reg->relative_target_port_id;
		add_len += reg->transport_id_len;
		if (add_len + sizeof(data->header) > data_len) {
			break;
		}
		memcpy(&desc->transport_id, reg->transport_id,
		       reg->transport_id_len);
		to_be32(&desc->desc_len, reg->transport_id_len);
	}
	to_be32(&data->header.addiontal_len, add_len);
	pthread_mutex_unlock(&dev->reservation_lock);

	return (sizeof(data->header) + add_len);
}

static int
spdk_scsi_pr_in_report_capabilities(struct spdk_scsi_task *task,
				    uint8_t *data, uint16_t data_len)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_scsi_dev *dev = lun->dev;
	struct spdk_scsi_pr_in_report_capabilities_data *param;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR IN REPORT CAPABILITIES\n");

	if (data_len < 8) {
		return -ENOMEM;
	}

	param = (struct spdk_scsi_pr_in_report_capabilities_data *)data;
	memset(param, 0, sizeof(*param));
	/* TODO: can support more capabilities bits */
	param->ptpl_c = (dev->pr_file == NULL ? false : true);
	if (param->ptpl_c) {
		param->ptpl_a = dev->ptpl_activated;
	}
	to_be16(&param->length, 8);
	param->tmv = true;
	param->wr_ex = true;
	param->ex_ac = true;
	param->wr_ex_ro = true;
	param->ex_ac_ro = true;
	param->wr_ex_ar = true;
	param->ex_ac_ar = true;

	return sizeof(*param);
}

static int
spdk_scsi_pr_in_read_reservations(struct spdk_scsi_task *task,
				  uint8_t *data, uint16_t data_len)
{
	struct spdk_scsi_lun *lun = task->lun;
	struct spdk_scsi_dev *dev = lun->dev;
	struct spdk_scsi_pr_in_read_reservations_data *param;
	bool all_regs = false;

	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR IN READ RESERVATIONS\n");

	if (data_len < sizeof(param->header)) {
		return -ENOMEM;
	}
	pthread_mutex_lock(&dev->reservation_lock);
	param = (struct spdk_scsi_pr_in_read_reservations_data *)(data);

	to_be32(&param->header.pr_generation, dev->pr_generation);
	if (dev->holder) {
		if (sizeof(*param) > data_len) {
			pthread_mutex_unlock(&dev->reservation_lock);
			return -ENOMEM;
		}
		all_regs = spdk_scsi_pr_is_all_registrants_type(dev);
		if (all_regs) {
			to_be64(&param->rkey, 0);
		} else {
			to_be64(&param->rkey, dev->crkey);
		}
		to_be32(&param->header.addiontal_len, 16);
		param->scope = SPDK_SCSI_PR_LU_SCOPE;
		param->type = dev->type;
		pthread_mutex_unlock(&dev->reservation_lock);
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "READ RESERVATIONS with valid reservation\n");
		return sizeof(*param);
	}

	/* no reservation */
	to_be32(&param->header.addiontal_len, 0);
	pthread_mutex_unlock(&dev->reservation_lock);
	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "READ RESERVATIONS no reservation\n");
	return sizeof(param->header);
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
	case SPDK_SCSI_PR_IN_READ_RESERVATION:
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
	struct spdk_scsi_lun *lun;
	struct spdk_scsi_dev *dev;
	uint8_t *cdb;
	enum spdk_scsi_pr_type_code type;
	enum spdk_scsi_pr_out_service_action_code action;
	struct spdk_scsi_pr_registrant *reg;
	bool dma_to_device = false;

	lun = task->lun;
	if (!lun) {
		return 0;
	}
	dev = lun->dev;
	if (!dev) {
		return 0;
	}
	cdb = task->cdb;
	/* no reservation holders */
	if (dev->holder == NULL) {
		return 0;
	}

	type = dev->type;
	assert(type != 0);

	reg = spdk_scsi_pr_get_registrant(dev, task->initiator_port, task->target_port);
	/* current I_T nexus hold the reservation */
	if (spdk_scsi_pr_registrant_is_holder(dev, reg)) {
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
		if (reg) {
			return 0;
		} else {
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
			if (reg) {
				return 0;
			} else {
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

	switch (type) {
	case SPDK_SCSI_PR_WRITE_EXCLUSIVE:
		if (dma_to_device) {
			SPDK_ERRLOG("CHECK: Write Exclusive reservation type "
				    "rejects command 0x%x\n", cdb[0]);
			goto conflict;
		} else {
			return 0;
		}
		break;
	case SPDK_SCSI_PR_EXCLUSIVE_ACCESS:
		SPDK_ERRLOG("CHECK: Exclusive Access reservation type "
			    "rejects command 0x%x\n", cdb[0]);
		goto conflict;
	case SPDK_SCSI_PR_WRITE_EXCLUSIVE_REGS_ONLY:
	case SPDK_SCSI_PR_WRITE_EXCLUSIVE_ALL_REGS:
		if (reg) {
			return 0;
		} else {
			if (dma_to_device) {
				SPDK_ERRLOG("CHECK: Registrants only reservation "
					    "type  reject command 0x%x\n", cdb[0]);
				goto conflict;
			}
			return 0;
		}
		break;
	case SPDK_SCSI_PR_EXCLUSIVE_ACCESS_REGS_ONLY:
	case SPDK_SCSI_PR_EXCLUSIVE_ACCESS_ALL_REGS:
		if (reg) {
			return 0;
		} else {
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

static void
spdk_scsi_dev_deactive_ptpl(struct spdk_scsi_dev *dev)
{
	free(dev->pr_file);
	dev->pr_file = NULL;
	if (dev->ptpl_activated) {
		dev->ptpl_activated = false;
	}
}

static int
spdk_scsi_dev_json_write_cb(void *cb_ctx, const void *data, size_t size)
{
	struct spdk_scsi_dev *dev = cb_ctx;
	size_t rc;
	FILE *fd;

	fd = fopen(dev->pr_file, "w");
	if (!fd) {
		SPDK_ERRLOG("Can't open file %s for write\n", dev->pr_file);
		spdk_scsi_dev_deactive_ptpl(dev);
		return -1;
	}
	rc = fwrite(data, 1, size, fd);
	fclose(fd);

	return rc == size ? 0 : -1;
}

static int
spdk_scsi_dev_save_reservation(struct spdk_scsi_dev *dev, bool aptpl)
{
	struct spdk_scsi_pr_registrant *reg;
	struct spdk_scsi_iscsi_transport_id *trans_id;
	struct spdk_json_write_ctx *w;
	int rc;

	w = spdk_json_write_begin(spdk_scsi_dev_json_write_cb, dev, 0);
	if (w == NULL) {
		return -ENOMEM;
	}
	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "name", dev->name);
	spdk_json_write_named_bool(w, "aptpl", aptpl);

	if (aptpl) {
		pthread_mutex_lock(&dev->reservation_lock);
		spdk_json_write_named_uint32(w, "type", dev->type);
		spdk_json_write_named_uint32(w, "generation", dev->pr_generation);
		spdk_json_write_named_uint64(w, "crkey", dev->crkey);

		spdk_json_write_named_array_begin(w, "registrants");
		TAILQ_FOREACH(reg, &dev->reg_head, link) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_bool(w, "holder",
						   spdk_scsi_pr_registrant_is_holder(dev, reg));
			spdk_json_write_named_uint64(w, "rkey", reg->rkey);
			trans_id = (struct spdk_scsi_iscsi_transport_id *)reg->transport_id;
			spdk_json_write_named_string(w, "transport_id_name", trans_id->name);
			spdk_json_write_named_string(w, "initiator_port_name",
						     reg->initiator_port_name);
			spdk_json_write_named_uint32(w, "relative_target_port_id",
						     reg->relative_target_port_id);
			spdk_json_write_named_string(w, "target_port_name",
						     reg->target_port_name);
			spdk_json_write_object_end(w);
		}
		spdk_json_write_array_end(w);
		pthread_mutex_unlock(&dev->reservation_lock);
	}
	spdk_json_write_object_end(w);
	rc = spdk_json_write_end(w);

	return rc;
}

static int
spdk_scsi_dev_update_aptpl(struct spdk_scsi_dev *dev,
			   struct spdk_scsi_task *task, bool aptpl)
{
	int rc;

	if (dev->pr_file == NULL && !aptpl) {
		return 0;
	}

	if (dev->pr_file == NULL && aptpl) {
		SPDK_ERRLOG("Users don't provide a persist file\n");
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_ILLEGAL_REQUEST,
					  SPDK_SCSI_ASC_INVALID_FIELD_IN_CDB,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
		return -EINVAL;
	}

	dev->ptpl_activated = aptpl;
	rc = spdk_scsi_dev_save_reservation(dev, aptpl);
	if (rc < 0) {
		spdk_scsi_task_set_status(task, SPDK_SCSI_STATUS_CHECK_CONDITION,
					  SPDK_SCSI_SENSE_NOT_READY,
					  SPDK_SCSI_ASC_NO_ADDITIONAL_SENSE,
					  SPDK_SCSI_ASCQ_CAUSE_NOT_REPORTABLE);
	}

	return rc;
}

#define MAX_PR_REGISTRANTS 1024

struct scsi_dev_pr_reg {
	bool holder;
	uint64_t rkey;
	uint32_t relative_target_port_id;
	char *transport_id_name;
	char *initiator_port_name;
	char *target_port_name;
};

struct scsi_dev_pr_regs {
	size_t num_regs;
	struct scsi_dev_pr_reg reg[MAX_PR_REGISTRANTS];
};

static const struct spdk_json_object_decoder dev_pr_reg_decoders[] = {
	{"holder", offsetof(struct scsi_dev_pr_reg, holder), spdk_json_decode_bool},
	{"rkey", offsetof(struct scsi_dev_pr_reg, rkey), spdk_json_decode_uint64},
	{
		"relative_target_port_id", offsetof(struct scsi_dev_pr_reg, relative_target_port_id),
		spdk_json_decode_uint32
	},
	{"initiator_port_name", offsetof(struct scsi_dev_pr_reg, initiator_port_name), spdk_json_decode_string},
	{"transport_id_name", offsetof(struct scsi_dev_pr_reg, transport_id_name), spdk_json_decode_string},
	{"target_port_name", offsetof(struct scsi_dev_pr_reg, target_port_name), spdk_json_decode_string},
};

static int
decode_pr_reg(const struct spdk_json_val *val, void *out)
{
	struct scsi_dev_pr_reg *reg = out;

	return spdk_json_decode_object(val, dev_pr_reg_decoders,
				       SPDK_COUNTOF(dev_pr_reg_decoders), reg);
}

static int
decode_pr_regs(const struct spdk_json_val *val, void *out)
{
	struct scsi_dev_pr_regs *regs = out;

	return spdk_json_decode_array(val, decode_pr_reg, regs->reg,
				      MAX_PR_REGISTRANTS,
				      &regs->num_regs, sizeof(struct scsi_dev_pr_reg));
}

struct scsi_dev_pr_node {
	char *name;
	bool aptpl;
	uint32_t type;
	uint32_t generation;
	uint64_t crkey;
	struct scsi_dev_pr_regs regs;
};

static const struct spdk_json_object_decoder dev_pr_node_decoders[] = {
	{"name", offsetof(struct scsi_dev_pr_node, name), spdk_json_decode_string},
	{"aptpl", offsetof(struct scsi_dev_pr_node, aptpl), spdk_json_decode_bool},
	{"type", offsetof(struct scsi_dev_pr_node, type), spdk_json_decode_uint32, true},
	{"generation", offsetof(struct scsi_dev_pr_node, generation), spdk_json_decode_uint32, true},
	{"crkey", offsetof(struct scsi_dev_pr_node, crkey), spdk_json_decode_uint64, true},
	{"registrants", offsetof(struct scsi_dev_pr_node, regs), decode_pr_regs, true},
};

static void
spdk_scsi_dev_restore_transport_id(struct spdk_scsi_pr_registrant *reg,
				   const char *transport_id_name)
{
	struct spdk_scsi_iscsi_transport_id *data;
	uint32_t len;
	char *name;

	memset(reg->transport_id, 0, sizeof(reg->transport_id));
	reg->transport_id_len = 0;

	data = (struct spdk_scsi_iscsi_transport_id *)reg->transport_id;

	data->protocol_id = (uint8_t)SPDK_SPC_PROTOCOL_IDENTIFIER_ISCSI;
	data->format = 0x1;

	name = data->name;
	len = snprintf(name, SPDK_SCSI_MAX_TRANSPORT_ID_LENGTH - 3, "%s", transport_id_name);
	do {
		name[len++] = '\0';
	} while (len & 3);

	to_be16(&data->additional_len, len);
	reg->transport_id_len = len + sizeof(*data);
}

static int
spdk_scsi_dev_restore_registrant(struct spdk_scsi_dev *dev,
				 enum spdk_scsi_pr_type_code type,
				 bool holder, uint64_t rkey,
				 const char *transport_id_name,
				 const char *initiator_port_name,
				 uint16_t relative_target_port_id,
				 const char *target_port_name)
{
	struct spdk_scsi_pr_registrant *reg;

	reg = calloc(1, sizeof(*reg));
	if (!reg) {
		return -ENOMEM;
	}

	reg->rkey = rkey;
	reg->relative_target_port_id = relative_target_port_id;
	spdk_scsi_dev_restore_transport_id(reg, transport_id_name);
	snprintf(reg->initiator_port_name, sizeof(reg->initiator_port_name), "%s",
		 initiator_port_name);
	snprintf(reg->target_port_name, sizeof(reg->target_port_name), "%s",
		 target_port_name);

	switch (type) {
	case SPDK_SCSI_PR_WRITE_EXCLUSIVE:
	case SPDK_SCSI_PR_EXCLUSIVE_ACCESS:
	case SPDK_SCSI_PR_WRITE_EXCLUSIVE_REGS_ONLY:
	case SPDK_SCSI_PR_EXCLUSIVE_ACCESS_REGS_ONLY:
		if (dev->holder && holder) {
			SPDK_ERRLOG("Holder is already exist\n");
			free(reg);
			return -EINVAL;
		} else if (!dev->holder && holder) {
			dev->holder = reg;
		}
		break;
	case SPDK_SCSI_PR_WRITE_EXCLUSIVE_ALL_REGS:
	case SPDK_SCSI_PR_EXCLUSIVE_ACCESS_ALL_REGS:
		if (!dev->holder && holder) {
			dev->holder = reg;
		}
		break;
	default:
		break;
	}

	TAILQ_INSERT_TAIL(&dev->reg_head, reg, link);
	return 0;
}

int
spdk_scsi_dev_load_reservation(struct spdk_scsi_dev *dev)
{
	size_t size;
	FILE *fd;
	void *buf;
	ssize_t rc;
	struct spdk_json_val *values;
	struct scsi_dev_pr_node pr_node = {0};
	uint32_t i;
	struct spdk_scsi_pr_registrant *reg;

	fd = fopen(dev->pr_file, "r");
	/* It's not an error if the file does not exist */
	if (!fd) {
		SPDK_NOTICELOG("File %s does not exist\n", dev->pr_file);
		return 0;
	}

	/* Load all persist file contents into a local buffer */
	buf = spdk_json_read_file_to_buf(fd, &size);
	fclose(fd);
	if (!buf) {
		SPDK_ERRLOG("Load persit file %s failed\n", dev->pr_file);
		spdk_scsi_dev_deactive_ptpl(dev);
		return -ENOMEM;
	}

	rc = spdk_json_parse_buf(&values, buf, size, 0);
	if (rc <= 0) {
		SPDK_ERRLOG("spdk_json_parse_buf failed, rc %d\n", (int)rc);
		free(buf);
		spdk_scsi_dev_deactive_ptpl(dev);
		return -EINVAL;
	}

	/* Decode json */
	if (spdk_json_decode_object(values, dev_pr_node_decoders,
				    SPDK_COUNTOF(dev_pr_node_decoders),
				    &pr_node)) {
		SPDK_ERRLOG("Invalid objects in the persist file %s, "
			    "disable ptpl feature\n", dev->pr_file);
		rc = -EINVAL;
		goto error;
	}

	if (pr_node.aptpl == 0 || pr_node.regs.num_regs == 0) {
		SPDK_NOTICELOG("PTPL is in deactive state\n");
		goto out;
	}

	/* Construct reservation and registrants */
	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR LOAD: Name: %s\n", pr_node.name);
	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR LOAD: Type %u\n", pr_node.type);
	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR LOAD: Generation %u\n", pr_node.generation);
	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR LOAD: Crkey 0x%"PRIx64"\n", pr_node.crkey);
	SPDK_DEBUGLOG(SPDK_LOG_SCSI, "PR LOAD: Number of Registrants 0x%u\n",
		      (uint32_t)pr_node.regs.num_regs);

	for (i = 0; i < (uint32_t)pr_node.regs.num_regs; i++) {
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REG LOAD: Holder %u\n", pr_node.regs.reg[i].holder);
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REG LOAD: Rkey 0x%"PRIx64"\n", pr_node.regs.reg[i].rkey);
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "Transport ID Name: %s\n", pr_node.regs.reg[i].transport_id_name);
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "Initiator Port Name: %s\n", pr_node.regs.reg[i].initiator_port_name);
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REG LOAD: Relative Target Port ID %u\n",
			      pr_node.regs.reg[i].relative_target_port_id);
		SPDK_DEBUGLOG(SPDK_LOG_SCSI, "REG LOAD: Target Port Name: %s\n",
			      pr_node.regs.reg[i].target_port_name);

		rc = spdk_scsi_dev_restore_registrant(dev, pr_node.type, pr_node.regs.reg[i].holder,
						      pr_node.regs.reg[i].rkey,
						      pr_node.regs.reg[i].transport_id_name,
						      pr_node.regs.reg[i].initiator_port_name,
						      pr_node.regs.reg[i].relative_target_port_id,
						      pr_node.regs.reg[i].target_port_name);
		if (rc != 0) {
			/* cleanup */
			TAILQ_FOREACH(reg, &dev->reg_head, link) {
				TAILQ_REMOVE(&dev->reg_head, reg, link);
				free(reg);
			}
			goto error;
		}
	}

	dev->type = pr_node.type;
	dev->crkey = pr_node.crkey;
	dev->pr_generation = pr_node.generation;
	dev->ptpl_activated = true;

out:
	free(buf);
	free(values);
	return 0;

error:
	free(buf);
	free(values);
	spdk_scsi_dev_deactive_ptpl(dev);
	return rc;
}
