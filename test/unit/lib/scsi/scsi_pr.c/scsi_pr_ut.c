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

#include "spdk/stdinc.h"

#include "scsi/port.c"
#include "scsi/scsi_pr.c"

#include "spdk_cunit.h"

#include "spdk_internal/mock.h"

SPDK_LOG_REGISTER_COMPONENT("scsi", SPDK_LOG_SCSI)

void
spdk_scsi_task_set_status(struct spdk_scsi_task *task, int sc, int sk,
			  int asc, int ascq)
{
	task->status = sc;
}

/*
 * Reservation Unit Test Configuration
 *
 *  --------      --------      -------
 * | Host A |    | Host B |    | Host C|
 *  --------      --------      -------
 *     |             |             |
 *   ------        ------        ------
 *  |Port A|      |Port B|      |Port C|
 *   ------        ------        ------
 *      \            |             /
 *       \           |            /
 *        \          |           /
 *        ------------------------
 *       |  Target Node 1 Port 0  |
 *        ------------------------
 *                   |
 *   ----------------------------------
 *  |           Target Node            |
 *   ----------------------------------
 *                  |
 *                -----
 *               |LUN 0|
 *                -----
 *
 */

static struct spdk_scsi_lun g_lun;
static struct spdk_scsi_port g_i_port_a;
static struct spdk_scsi_port g_i_port_b;
static struct spdk_scsi_port g_i_port_c;
static struct spdk_scsi_port g_t_port_0;

static void
ut_lun_deinit(void)
{
	struct spdk_scsi_pr_registrant *reg, *tmp;

	TAILQ_FOREACH_SAFE(reg, &g_lun.reg_head, link, tmp) {
		TAILQ_REMOVE(&g_lun.reg_head, reg, link);
		free(reg);
	}
	g_lun.reservation.rtype = 0;
	g_lun.reservation.crkey = 0;
	g_lun.reservation.holder = NULL;
	g_lun.pr_generation = 0;
}

static void
ut_port_init(void)
{
	int rc;

	/* g_i_port_a */
	rc = spdk_scsi_port_construct(&g_i_port_a, 0xa, 0,
				      "iqn.2016-06.io.spdk:fe5aacf7420a,i,0x00023d00000a");
	SPDK_CU_ASSERT_FATAL(rc == 0);
	spdk_scsi_port_set_iscsi_transport_id(&g_i_port_a,
					      "iqn.2016-06.io.spdk:fe5aacf7420a", 0x00023d00000a);
	/* g_i_port_b */
	rc = spdk_scsi_port_construct(&g_i_port_b, 0xb, 0,
				      "iqn.2016-06.io.spdk:fe5aacf7420b,i,0x00023d00000b");
	SPDK_CU_ASSERT_FATAL(rc == 0);
	spdk_scsi_port_set_iscsi_transport_id(&g_i_port_b,
					      "iqn.2016-06.io.spdk:fe5aacf7420b", 0x00023d00000b);
	/* g_i_port_c */
	rc = spdk_scsi_port_construct(&g_i_port_c, 0xc, 0,
				      "iqn.2016-06.io.spdk:fe5aacf7420c,i,0x00023d00000c");
	SPDK_CU_ASSERT_FATAL(rc == 0);
	spdk_scsi_port_set_iscsi_transport_id(&g_i_port_c,
					      "iqn.2016-06.io.spdk:fe5aacf7420c", 0x00023d00000c);
	/* g_t_port_0 */
	rc = spdk_scsi_port_construct(&g_t_port_0, 0x0, 1,
				      "iqn.2016-06.io.spdk:fe5aacf74200,t,0x00023d000000");
	SPDK_CU_ASSERT_FATAL(rc == 0);
	spdk_scsi_port_set_iscsi_transport_id(&g_t_port_0,
					      "iqn.2016-06.io.spdk:fe5aacf74200", 0x00023d000000);
}

static void
ut_lun_init(void)
{
	TAILQ_INIT(&g_lun.reg_head);
}

static void
ut_init_reservation_test(void)
{
	ut_lun_init();
	ut_port_init();
	ut_lun_init();
}

static void
ut_deinit_reservation_test(void)
{
	ut_lun_deinit();
}

/* Host A: register with key 0xa.
 * Host B: register with key 0xb.
 * Host C: register with key 0xc.
 */
static void
test_build_registrants(void)
{
	struct spdk_scsi_pr_registrant *reg;
	struct spdk_scsi_task task = {0};
	uint32_t gen;
	int rc;

	task.lun = &g_lun;
	task.target_port = &g_t_port_0;

	gen = g_lun.pr_generation;

	/* I_T nexus: Initiator Port A to Target Port 0 */
	task.initiator_port = &g_i_port_a;
	/* Test Case: Host A registers with a new key */
	task.status = 0;
	rc = spdk_scsi_pr_out_register(&task, SPDK_SCSI_PR_OUT_REGISTER,
				       0x0, 0xa1, 0, 0, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	reg = spdk_scsi_pr_get_registrant(&g_lun, &g_i_port_a, &g_t_port_0);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xa1);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation == gen + 1);

	/* Test Case: Host A replaces with a new key */
	task.status = 0;
	rc = spdk_scsi_pr_out_register(&task, SPDK_SCSI_PR_OUT_REGISTER,
				       0xa1, 0xa, 0, 0, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	reg = spdk_scsi_pr_get_registrant(&g_lun, &g_i_port_a, &g_t_port_0);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xa);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation == gen + 2);

	/* Test Case: Host A replaces with a new key, reservation conflict is expected */
	task.status = 0;
	rc = spdk_scsi_pr_out_register(&task, SPDK_SCSI_PR_OUT_REGISTER,
				       0xa1, 0xdead, 0, 0, 0);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	reg = spdk_scsi_pr_get_registrant(&g_lun, &g_i_port_a, &g_t_port_0);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xa);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation == gen + 2);
	SPDK_CU_ASSERT_FATAL(task.status == SPDK_SCSI_STATUS_RESERVATION_CONFLICT);

	/* I_T nexus: Initiator Port B to Target Port 0 */
	task.initiator_port = &g_i_port_b;
	/* Test Case: Host B registers with a new key */
	task.status = 0;
	rc = spdk_scsi_pr_out_register(&task, SPDK_SCSI_PR_OUT_REGISTER,
				       0x0, 0xb, 0, 0, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	reg = spdk_scsi_pr_get_registrant(&g_lun, &g_i_port_b, &g_t_port_0);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xb);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation == gen + 3);

	/* I_T nexus: Initiator Port C to Target Port 0 */
	task.initiator_port = &g_i_port_c;
	/* Test Case: Host C registers with a new key */
	task.status = 0;
	rc = spdk_scsi_pr_out_register(&task, SPDK_SCSI_PR_OUT_REGISTER,
				       0x0, 0xc, 0, 0, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	reg = spdk_scsi_pr_get_registrant(&g_lun, &g_i_port_c, &g_t_port_0);
	SPDK_CU_ASSERT_FATAL(reg != NULL);
	SPDK_CU_ASSERT_FATAL(reg->rkey == 0xc);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation == gen + 4);
}

static void
test_reservation_register(void)
{
	ut_init_reservation_test();

	test_build_registrants();

	ut_deinit_reservation_test();
}

static void
test_reservation_reserve(void)
{
	struct spdk_scsi_pr_registrant *reg;
	struct spdk_scsi_task task = {0};
	uint32_t gen;
	int rc;

	task.lun = &g_lun;
	task.target_port = &g_t_port_0;

	ut_init_reservation_test();
	test_build_registrants();

	gen = g_lun.pr_generation;

	task.initiator_port = &g_i_port_a;
	task.status = 0;
	/* Test Case: Host A acquires the reservation */
	rc = spdk_scsi_pr_out_reserve(&task, SPDK_SCSI_PR_WRITE_EXCLUSIVE,
				      0xa, 0, 0, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.rtype == SPDK_SCSI_PR_WRITE_EXCLUSIVE);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.crkey == 0xa);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation == gen);

	/* Test Case: Host B acquires the reservation, reservation
	 * conflict is expected.
	 */
	task.initiator_port = &g_i_port_b;
	task.status = 0;
	rc = spdk_scsi_pr_out_reserve(&task, SPDK_SCSI_PR_WRITE_EXCLUSIVE,
				      0xb, 0, 0, 0);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(task.status == SPDK_SCSI_STATUS_RESERVATION_CONFLICT);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.rtype == SPDK_SCSI_PR_WRITE_EXCLUSIVE);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.crkey == 0xa);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation == gen);

	/* Test Case: Host A unregister with reservation */
	task.initiator_port = &g_i_port_a;
	task.status = 0;
	rc = spdk_scsi_pr_out_register(&task, SPDK_SCSI_PR_OUT_REGISTER,
				       0xa, 0, 0, 0, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.rtype == 0);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.crkey == 0);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation == gen + 1);
	reg = spdk_scsi_pr_get_registrant(&g_lun, &g_i_port_a, &g_t_port_0);
	SPDK_CU_ASSERT_FATAL(reg == NULL);

	/* Test Case: Host B acquires the reservation */
	task.initiator_port = &g_i_port_b;
	task.status = 0;
	rc = spdk_scsi_pr_out_reserve(&task, SPDK_SCSI_PR_WRITE_EXCLUSIVE_ALL_REGS,
				      0xb, 0, 0, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.rtype == SPDK_SCSI_PR_WRITE_EXCLUSIVE_ALL_REGS);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation == gen + 1);

	/* Test Case: Host C acquires the reservation with invalid type */
	task.initiator_port = &g_i_port_c;
	task.status = 0;
	rc = spdk_scsi_pr_out_reserve(&task, SPDK_SCSI_PR_WRITE_EXCLUSIVE,
				      0xc, 0, 0, 0);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(task.status == SPDK_SCSI_STATUS_RESERVATION_CONFLICT);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.rtype == SPDK_SCSI_PR_WRITE_EXCLUSIVE_ALL_REGS);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation == gen + 1);

	/* Test Case: Host C acquires the reservation, all registrants type */
	task.status = 0;
	rc = spdk_scsi_pr_out_reserve(&task, SPDK_SCSI_PR_WRITE_EXCLUSIVE_ALL_REGS,
				      0xc, 0, 0, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.rtype == SPDK_SCSI_PR_WRITE_EXCLUSIVE_ALL_REGS);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation == gen + 1);

	ut_deinit_reservation_test();
}

static void
test_reservation_preempt_non_all_regs(void)
{
	struct spdk_scsi_pr_registrant *reg;
	struct spdk_scsi_task task = {0};
	uint32_t gen;
	int rc;

	task.lun = &g_lun;
	task.target_port = &g_t_port_0;

	ut_init_reservation_test();
	test_build_registrants();

	task.initiator_port = &g_i_port_a;
	task.status = 0;
	gen = g_lun.pr_generation;
	/* Host A acquires the reservation */
	rc = spdk_scsi_pr_out_reserve(&task, SPDK_SCSI_PR_WRITE_EXCLUSIVE_REGS_ONLY,
				      0xa, 0, 0, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.rtype == SPDK_SCSI_PR_WRITE_EXCLUSIVE_REGS_ONLY);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.crkey == 0xa);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation == gen);

	/* Test Case: Host B premmpts Host A, Check condition is expected
	 * for zeroed service action reservation key */
	task.initiator_port = &g_i_port_b;
	task.status = 0;
	rc = spdk_scsi_pr_out_preempt(&task, SPDK_SCSI_PR_OUT_PREEMPT,
				      SPDK_SCSI_PR_WRITE_EXCLUSIVE_REGS_ONLY,
				      0xb, 0);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(task.status == SPDK_SCSI_STATUS_CHECK_CONDITION);

	/* Test Case: Host B preempts Host A, Host A is unregisted */
	task.status = 0;
	gen = g_lun.pr_generation;
	rc = spdk_scsi_pr_out_preempt(&task, SPDK_SCSI_PR_OUT_PREEMPT,
				      SPDK_SCSI_PR_WRITE_EXCLUSIVE,
				      0xb, 0xa);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.rtype == SPDK_SCSI_PR_WRITE_EXCLUSIVE);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.crkey == 0xb);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation > gen);
	reg = spdk_scsi_pr_get_registrant(&g_lun, &g_i_port_a, &g_t_port_0);
	SPDK_CU_ASSERT_FATAL(reg == NULL);

	/* Test Case: Host B preempts itself */
	task.status = 0;
	gen = g_lun.pr_generation;
	rc = spdk_scsi_pr_out_preempt(&task, SPDK_SCSI_PR_OUT_PREEMPT,
				      SPDK_SCSI_PR_WRITE_EXCLUSIVE,
				      0xb, 0xb);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.rtype == SPDK_SCSI_PR_WRITE_EXCLUSIVE);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.crkey == 0xb);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation > gen);

	/* Test Case: Host B preempts itself and remove registrants */
	task.status = 0;
	gen = g_lun.pr_generation;
	rc = spdk_scsi_pr_out_preempt(&task, SPDK_SCSI_PR_OUT_PREEMPT,
				      SPDK_SCSI_PR_WRITE_EXCLUSIVE,
				      0xb, 0xc);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.rtype == SPDK_SCSI_PR_WRITE_EXCLUSIVE);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.crkey == 0xb);
	reg = spdk_scsi_pr_get_registrant(&g_lun, &g_i_port_c, &g_t_port_0);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation > gen);

	ut_deinit_reservation_test();
}

static void
test_reservation_preempt_all_regs(void)
{
	struct spdk_scsi_pr_registrant *reg;
	struct spdk_scsi_task task = {0};
	uint32_t gen;
	int rc;

	task.lun = &g_lun;
	task.target_port = &g_t_port_0;

	ut_init_reservation_test();
	test_build_registrants();

	/* Test Case: No reservation yet, Host B removes Host C's registrant */
	task.initiator_port = &g_i_port_b;
	task.status = 0;
	gen = g_lun.pr_generation;
	rc = spdk_scsi_pr_out_preempt(&task, SPDK_SCSI_PR_OUT_PREEMPT,
				      SPDK_SCSI_PR_WRITE_EXCLUSIVE_REGS_ONLY,
				      0xb, 0xc);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	reg = spdk_scsi_pr_get_registrant(&g_lun, &g_i_port_c, &g_t_port_0);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation > gen);

	task.initiator_port = &g_i_port_a;
	task.status = 0;
	gen = g_lun.pr_generation;
	/* Host A acquires the reservation */
	rc = spdk_scsi_pr_out_reserve(&task, SPDK_SCSI_PR_WRITE_EXCLUSIVE_ALL_REGS,
				      0xa, 0, 0, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.rtype == SPDK_SCSI_PR_WRITE_EXCLUSIVE_ALL_REGS);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation == gen);

	/* Test Case: Host B removes Host A's registrant and preempt */
	task.initiator_port = &g_i_port_b;
	task.status = 0;
	gen = g_lun.pr_generation;
	rc = spdk_scsi_pr_out_preempt(&task, SPDK_SCSI_PR_OUT_PREEMPT,
				      SPDK_SCSI_PR_EXCLUSIVE_ACCESS_ALL_REGS,
				      0xb, 0x0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	reg = spdk_scsi_pr_get_registrant(&g_lun, &g_i_port_a, &g_t_port_0);
	SPDK_CU_ASSERT_FATAL(reg == NULL);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.rtype == SPDK_SCSI_PR_EXCLUSIVE_ACCESS_ALL_REGS);
	SPDK_CU_ASSERT_FATAL(g_lun.pr_generation > gen);

	ut_deinit_reservation_test();
}

static void
test_reservation_cmds_conflict(void)
{
	struct spdk_scsi_pr_registrant *reg;
	struct spdk_scsi_task task = {0};
	uint8_t cdb[32];
	int rc;

	task.lun = &g_lun;
	task.target_port = &g_t_port_0;
	task.cdb = cdb;

	ut_init_reservation_test();
	test_build_registrants();

	/* Host A acquires the reservation */
	task.initiator_port = &g_i_port_a;
	rc = spdk_scsi_pr_out_reserve(&task, SPDK_SCSI_PR_WRITE_EXCLUSIVE_REGS_ONLY,
				      0xa, 0, 0, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.rtype == SPDK_SCSI_PR_WRITE_EXCLUSIVE_REGS_ONLY);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.crkey == 0xa);

	/* Remove Host B registrant */
	task.initiator_port = &g_i_port_b;
	task.status = 0;
	rc = spdk_scsi_pr_out_register(&task, SPDK_SCSI_PR_OUT_REGISTER,
				       0xb, 0, 0, 0, 0);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	reg = spdk_scsi_pr_get_registrant(&g_lun, &g_i_port_b, &g_t_port_0);
	SPDK_CU_ASSERT_FATAL(reg == NULL);

	/* Test Case: Host B sends Read/Write commands,
	 * reservation conflict is expected.
	 */
	task.cdb[0] = SPDK_SBC_READ_10;
	task.status = 0;
	rc = spdk_scsi_pr_check(&task);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	task.cdb[0] = SPDK_SBC_WRITE_10;
	task.status = 0;
	rc = spdk_scsi_pr_check(&task);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(task.status == SPDK_SCSI_STATUS_RESERVATION_CONFLICT);

	/* Test Case: Host C sends Read/Write commands */
	task.initiator_port = &g_i_port_c;
	task.cdb[0] = SPDK_SBC_READ_10;
	task.status = 0;
	rc = spdk_scsi_pr_check(&task);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	task.cdb[0] = SPDK_SBC_WRITE_10;
	task.status = 0;
	rc = spdk_scsi_pr_check(&task);
	SPDK_CU_ASSERT_FATAL(rc == 0);

	/* Host A preempts itself with SPDK_SCSI_PR_EXCLUSIVE_ACCESS */
	task.initiator_port = &g_i_port_a;
	rc = spdk_scsi_pr_out_preempt(&task, SPDK_SCSI_PR_OUT_PREEMPT,
				      SPDK_SCSI_PR_EXCLUSIVE_ACCESS,
				      0xa, 0xa);
	SPDK_CU_ASSERT_FATAL(rc == 0);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.rtype == SPDK_SCSI_PR_EXCLUSIVE_ACCESS);
	SPDK_CU_ASSERT_FATAL(g_lun.reservation.crkey == 0xa);

	/* Test Case: Host C sends Read/Write commands */
	task.initiator_port = &g_i_port_c;
	task.cdb[0] = SPDK_SBC_READ_10;
	task.status = 0;
	rc = spdk_scsi_pr_check(&task);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(task.status == SPDK_SCSI_STATUS_RESERVATION_CONFLICT);
	task.cdb[0] = SPDK_SBC_WRITE_10;
	task.status = 0;
	rc = spdk_scsi_pr_check(&task);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(task.status == SPDK_SCSI_STATUS_RESERVATION_CONFLICT);

	/* Test Case: Host B sends Read/Write commands */
	task.initiator_port = &g_i_port_b;
	task.cdb[0] = SPDK_SBC_READ_10;
	task.status = 0;
	rc = spdk_scsi_pr_check(&task);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(task.status == SPDK_SCSI_STATUS_RESERVATION_CONFLICT);
	task.cdb[0] = SPDK_SBC_WRITE_10;
	task.status = 0;
	rc = spdk_scsi_pr_check(&task);
	SPDK_CU_ASSERT_FATAL(rc < 0);
	SPDK_CU_ASSERT_FATAL(task.status == SPDK_SCSI_STATUS_RESERVATION_CONFLICT);

	ut_deinit_reservation_test();
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("reservation_suite", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (CU_add_test(suite, "register", test_reservation_register) == NULL ||
	    CU_add_test(suite, "reserve", test_reservation_reserve) == NULL ||
	    CU_add_test(suite, "preempt", test_reservation_preempt_non_all_regs) == NULL ||
	    CU_add_test(suite, "preempt all regs", test_reservation_preempt_all_regs) == NULL ||
	    CU_add_test(suite, "conflict", test_reservation_cmds_conflict) == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;

}
