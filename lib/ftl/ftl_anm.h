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

#ifndef FTL_ANM_H
#define FTL_ANM_H

#include "spdk/thread.h"
#include "ftl_ppa.h"

struct ftl_nvme_ctrlr;
struct ftl_anm_event;
struct spdk_ftl_dev;

typedef void (*ftl_anm_fn)(struct ftl_anm_event *event);

enum ftl_anm_range {
	FTL_ANM_RANGE_LBK,
	FTL_ANM_RANGE_CHK,
	FTL_ANM_RANGE_PU,
	FTL_ANM_RANGE_MAX,
};

struct ftl_anm_event {
	/* Owner device */
	struct spdk_ftl_dev		*dev;

	/* Start PPA */
	struct ftl_ppa			ppa;

	/* Number of logical blocks */
	size_t				num_lbks;
};

int	ftl_anm_init(struct spdk_thread *thread, spdk_ftl_fn cb, void *cb_arg);
int	ftl_anm_free(spdk_ftl_fn cb, void *cb_arg);
int	ftl_anm_register_device(struct spdk_ftl_dev *dev, ftl_anm_fn fn);
void	ftl_anm_unregister_device(struct spdk_ftl_dev *dev);
void	ftl_anm_event_complete(struct ftl_anm_event *event);

#endif /* FTL_ANM_H */
