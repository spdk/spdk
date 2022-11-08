/*
 *   Copyright © 2022 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
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

#ifndef _VRDMA_IO_MGR_H
#define _VRDMA_IO_MGR_H
#include <stdio.h>
#include <stdint.h>
#include "spdk/vrdma_srv.h"
#include "spdk/vrdma_controller.h"
#include "snap_dma.h"

/**
 * enum vrdma_vq_wqe_sm_op_status - status of last operation
 * @VRDMA_WQE_SM_OP_OK:		Last operation finished without a problem
 * @VRDMA_WQE_SM_OP_ERR:	Last operation failed
 *
 * State machine operates asynchronously, usually by calling a function
 * and providing a callback. Once callback is called it calls the state machine
 * progress again and provides it with the status of the function called.
 * This enum describes the status of the function called.
 */
enum vrdma_qp_sm_op_status {
	VRDMA_QP_SM_OP_OK,
	VRDMA_QP_SM_OP_ERR,
};

struct vrdma_qp_sm_state {
	bool (*sm_handler)(struct spdk_vrdma_qp *vqp,
			enum vrdma_qp_sm_op_status status);
};

struct vrdma_qp_state_machine {
	struct vrdma_qp_sm_state *sm_array;
	uint16_t sme;
};


struct vrdma_sq_sm_state {
	bool (*sm_handler)(struct vrdma_sq *sq,
			enum vrdma_qp_sm_op_status status);
};

struct vrdma_sq_state_machine {
	struct vrdma_sq_sm_state *sm_array;
	uint16_t sme;
};

struct vrdma_rq_sm_state {
	bool (*sm_handler)(struct vrdma_rq *rq,
			enum vrdma_qp_sm_op_status status);
};

struct vrdma_rq_state_machine {
	struct vrdma_rq_sm_state *sm_array;
	uint16_t sme;
};

size_t spdk_io_mgr_get_num_threads(void);
struct spdk_thread *spdk_io_mgr_get_thread(int id);

int spdk_io_mgr_init(void);
void spdk_io_mgr_clear(void);

void vrdma_qp_sm_dma_cb(struct snap_dma_completion *self, int status);
void vrdma_qp_sm_init(struct spdk_vrdma_qp *vqp);
#endif
