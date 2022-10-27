/*-
 *   BSD LICENSE
 *
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
#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/conf.h"
#include "spdk/log.h"
#include "spdk/vrdma_admq.h"
#include "spdk/vrdma_srv.h"
#include "spdk/vrdma_controller.h"

static int vrdma_srv_device_notify(struct vrdma_dev *rdev)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_notify");
	return 0;
}

static int vrdma_srv_device_query_gid(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_vrdma_device_query_gid");
	return 0;
}

static int vrdma_srv_device_modify_gid(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd,
					struct vrdma_cmd_param *param)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_modify_gid");
	return 0;
}

static int vrdma_srv_device_create_eq(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_create_eq");
	return 0;
}

static int vrdma_srv_device_modify_eq(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_modify_eq");
	return 0;
}

static int vrdma_srv_device_destroy_eq(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_destroy_eq");
	return 0;
}

static int vrdma_srv_device_create_pd(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd, 
					struct vrdma_cmd_param *param)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_create_pd");
	return 0;
}

static int vrdma_srv_device_destroy_pd(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_destroy_pd");
	return 0;
}

static int vrdma_srv_device_create_mr(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd, 
					struct vrdma_cmd_param *param)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_create_mr");
	return 0;
}

static int vrdma_srv_device_destroy_mr(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd, 
					struct vrdma_cmd_param *param)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_destroy_mr");
	return 0;
}

static int vrdma_srv_device_create_cq(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_create_cq");
	return 0;
}

static int vrdma_srv_device_destroy_cq(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_destroy_cq");
	return 0;
}

static int vrdma_srv_device_create_qp(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_create_qp");
	return 0;
}

static int vrdma_srv_device_destroy_qp(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_destroy_qp");
	return 0;
}

static int vrdma_srv_device_query_qp(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd, 
					struct vrdma_cmd_param *param)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_query_qp");
	return 0;
}

static int vrdma_srv_device_modify_qp(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_modify_qp");
	return 0;
}

static int vrdma_srv_device_create_ah(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_create_ah");
	return 0;
}

static int vrdma_srv_device_destroy_ah(struct vrdma_dev *rdev, 
					struct vrdma_admin_cmd_entry *cmd)
{
	//TODO
	SPDK_NOTICELOG("lizh dummy function vrdma_srv_device_create_ah");
	return 0;
}

static const struct vRdmaServiceOps vrdma_srv_ops = {
	.vrdma_device_notify = vrdma_srv_device_notify,
	.vrdma_device_query_gid = vrdma_srv_device_query_gid,
	.vrdma_device_modify_gid = vrdma_srv_device_modify_gid,
	.vrdma_device_create_eq = vrdma_srv_device_create_eq,
	.vrdma_device_modify_eq = vrdma_srv_device_modify_eq,
	.vrdma_device_destroy_eq = vrdma_srv_device_destroy_eq,
	.vrdma_device_create_pd = vrdma_srv_device_create_pd,
	.vrdma_device_destroy_pd = vrdma_srv_device_destroy_pd,
	.vrdma_device_create_mr = vrdma_srv_device_create_mr,
	.vrdma_device_destroy_mr = vrdma_srv_device_destroy_mr,
	.vrdma_device_create_cq = vrdma_srv_device_create_cq,
	.vrdma_device_destroy_cq = vrdma_srv_device_destroy_cq,
	.vrdma_device_create_qp = vrdma_srv_device_create_qp,
	.vrdma_device_destroy_qp = vrdma_srv_device_destroy_qp,
	.vrdma_device_query_qp = vrdma_srv_device_query_qp,
	.vrdma_device_modify_qp = vrdma_srv_device_modify_qp,
	.vrdma_device_create_ah = vrdma_srv_device_create_ah,
	.vrdma_device_destroy_ah = vrdma_srv_device_destroy_ah,
};

void vrdma_srv_device_init(struct vrdma_ctrl *ctrl)
{
	ctrl->srv_ops = &vrdma_srv_ops;
}
