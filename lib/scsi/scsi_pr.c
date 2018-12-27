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
		/* Add a new registrant for the I_T nexus */
		rc = spdk_scsi_pr_register_registrant(dev, task->initiator_port,
						      task->target_port, sa_rkey);
		pthread_mutex_unlock(&dev->reservation_lock);
		return rc;
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
	return 0;
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
