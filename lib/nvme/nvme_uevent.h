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

/** \file
 * SPDK uevent
 */

#include "spdk/env.h"
#include "spdk/nvmf_spec.h"

#ifndef SPDK_UEVENT_H_
#define SPDK_UEVENT_H_

#define SPDK_NVME_UEVENT_SUBSYSTEM_UNRECOGNIZED 0
#define SPDK_NVME_UEVENT_SUBSYSTEM_UIO 1
#define SPDK_NVME_UEVENT_SUBSYSTEM_VFIO 2

enum spdk_nvme_uevent_action {
	SPDK_NVME_UEVENT_ADD = 0,
	SPDK_NVME_UEVENT_REMOVE = 1,
};

struct spdk_uevent {
	enum spdk_nvme_uevent_action action;
	int subsystem;
	char traddr[SPDK_NVMF_TRADDR_MAX_LEN + 1];
};

int nvme_uevent_connect(void);
int nvme_get_uevent(int fd, struct spdk_uevent *uevent);

#endif /* SPDK_UEVENT_H_ */
