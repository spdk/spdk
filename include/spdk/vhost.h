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

/**
 *  \file
 *  SPDK vhost
 */

#ifndef SPDK_VHOST_H
#define SPDK_VHOST_H

#include "spdk/stdinc.h"

#include "spdk/event.h"

#define SPDK_VHOST_SCSI_CTRLR_MAX_DEVS 8

/**
 * \param event event object. event arg1 is optional path to vhost socket.
 */
void spdk_vhost_startup(void *arg1, void *arg2);
void spdk_vhost_shutdown_cb(void);

/* Forward declaration */
struct spdk_vhost_scsi_ctrlr;

/**
 * Get handle to next controller.
 * \param prev Previous controller or NULL to get first one.
 * \return handle to next controller ot NULL if prev was the last one.
 */
struct spdk_vhost_scsi_ctrlr *spdk_vhost_scsi_ctrlr_next(struct spdk_vhost_scsi_ctrlr *prev);

const char *spdk_vhost_scsi_ctrlr_get_name(struct spdk_vhost_scsi_ctrlr *ctrl);
uint64_t spdk_vhost_scsi_ctrlr_get_cpumask(struct spdk_vhost_scsi_ctrlr *ctrl);
int spdk_vhost_scsi_ctrlr_construct(const char *name, uint64_t cpumask);
int spdk_vhost_parse_core_mask(const char *mask, uint64_t *cpumask);
struct spdk_scsi_dev *spdk_vhost_scsi_ctrlr_get_dev(struct spdk_vhost_scsi_ctrlr *ctrl,
		uint8_t num);
int spdk_vhost_scsi_ctrlr_add_dev(const char *name, unsigned scsi_dev_num, const char *lun_name);

#endif /* SPDK_VHOST_H */
