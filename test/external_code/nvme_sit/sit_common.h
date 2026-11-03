/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 SanDisk. All rights reserved.
 */

#ifndef SIT_COMMON_H
#define SIT_COMMON_H

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/string.h"

#define SIT_MAX_CTRLRS		16

/* Common I/O buffer constants used across multiple test files */
#define SIT_IO_SIZE			4096
#define SIT_IO_ALIGN		4096
#define SIT_MAX_POLL_ITERS		100000
#define SIT_DRAIN_TIMEOUT_SEC		30

/*
 * Outcome sentinel: a test may return SIT_SKIP from setup or run when the
 * current hardware cannot support the workload (e.g. MDTS/LBA geometry). The
 * runner reports SKIP rather than FAIL. Value follows the automake convention
 * and stays clear of 0 and negative errno returns.
 */
#define SIT_SKIP 77

struct sit_context {
	/* Attached controllers (populated during probe) */
	struct spdk_nvme_ctrlr *ctrlrs[SIT_MAX_CTRLRS];
	int                      num_ctrlrs;

	/* BDF allowlist from --bdf arguments (points into argv, no copy needed) */
	const char *bdf_filters[SIT_MAX_CTRLRS];
	int			num_bdf_filters;
	bool verbose;
};

/* Modular Test Interface */
typedef int (*sit_test_func)(struct sit_context *ctx);

struct sit_test_def {
	const char *name;
	sit_test_func setup;    /* Optional: Allocate resources */
	sit_test_func run;      /* Required: The Test Logic */
	sit_test_func teardown; /* Optional: Cleanup */
	struct sit_test_def *next;
};

/* Internal test registration hook */
void sit_register_test(struct sit_test_def *test);

#define SIT_REGISTER_TEST_ENTRY(_name, _run, _setup, _teardown) \
	static struct sit_test_def _test_def_##_name = { \
		.name = #_name, \
		.run = _run, \
		.setup = _setup, \
		.teardown = _teardown, \
	}; \
	static void __attribute__((constructor)) _register_##_name(void) { \
		sit_register_test(&_test_def_##_name); \
	}

/* Shared helper: drain all outstanding I/Os on qpair */
int sit_drain_qpair(struct spdk_nvme_qpair *qpair);

/* Shared helper: poll admin completions until *done is true */
int sit_drain_admin(struct spdk_nvme_ctrlr *ctrlr, bool *done);

/* Shared helper: look up first active namespace and compute I/O sizing */
int sit_get_ns_and_sizes(struct spdk_nvme_ctrlr *ctrlr,
			 struct spdk_nvme_ns **ns_out, uint32_t *nsid_out,
			 uint32_t *lba_size_out, uint32_t *nlb_out);

struct sit_io_ctx {
	bool				 done;
	bool				 error;
	uint32_t			 completions;
};

/* Generic I/O completion callback for sit_io_ctx */
void sit_io_cb(void *arg, const struct spdk_nvme_cpl *cpl);

/* Environment lifecycle -- called by tests that manage their own cycles */
int sit_env_init(void);
void sit_env_fini(void);

#endif /* SIT_COMMON_H */
