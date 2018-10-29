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

#ifndef OCSSD_ANM_H
#define OCSSD_ANM_H

#include "ocssd_ppa.h"

struct ocssd_nvme_ctrlr;
struct ocssd_anm_event;
struct ocssd_dev;

typedef void (*ocssd_anm_fn)(struct ocssd_anm_event *event);

enum ocssd_anm_range {
	OCSSD_ANM_RANGE_LBK,
	OCSSD_ANM_RANGE_CHK,
	OCSSD_ANM_RANGE_PU,
	OCSSD_ANM_RANGE_MAX,
};

struct ocssd_anm_event {
	/* Owner device */
	struct ocssd_dev			*dev;

	/* Start PPA */
	struct ocssd_ppa			ppa;

	/* ANM range */
	enum ocssd_anm_range			range;
};

int	ocssd_anm_init(void);
void	ocssd_anm_free(void);
int	ocssd_anm_register_device(struct ocssd_dev *dev, ocssd_anm_fn fn);
void	ocssd_anm_unregister_device(struct ocssd_dev *dev);
int	ocssd_anm_register_ctrlr(struct ocssd_nvme_ctrlr *ctrlr);
void	ocssd_anm_unregister_ctrlr(struct ocssd_nvme_ctrlr *ctrlr);
void	ocssd_anm_event_complete(struct ocssd_anm_event *event);

#endif /* OCSSD_ANM_H */
