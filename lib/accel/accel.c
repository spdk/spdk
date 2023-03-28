/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk_internal/accel_module.h"

#include "accel_internal.h"

#include "spdk/dma.h"
#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/json.h"
#include "spdk/crc32.h"
#include "spdk/util.h"
#include "spdk/hexlify.h"

/* Accelerator Framework: The following provides a top level
 * generic API for the accelerator functions defined here. Modules,
 * such as the one in /module/accel/ioat, supply the implementation
 * with the exception of the pure software implementation contained
 * later in this file.
 */

#define ALIGN_4K			0x1000
#define MAX_TASKS_PER_CHANNEL		0x800
#define ACCEL_SMALL_CACHE_SIZE		0
#define ACCEL_LARGE_CACHE_SIZE		0
/* Set MSB, so we don't return NULL pointers as buffers */
#define ACCEL_BUFFER_BASE		((void *)(1ull << 63))
#define ACCEL_BUFFER_OFFSET_MASK	((uintptr_t)ACCEL_BUFFER_BASE - 1)

struct accel_module {
	struct spdk_accel_module_if	*module;
	bool				supports_memory_domains;
};

/* Largest context size for all accel modules */
static size_t g_max_accel_module_size = sizeof(struct spdk_accel_task);

static struct spdk_accel_module_if *g_accel_module = NULL;
static spdk_accel_fini_cb g_fini_cb_fn = NULL;
static void *g_fini_cb_arg = NULL;
static bool g_modules_started = false;
static struct spdk_memory_domain *g_accel_domain;

/* Global list of registered accelerator modules */
static TAILQ_HEAD(, spdk_accel_module_if) spdk_accel_module_list =
	TAILQ_HEAD_INITIALIZER(spdk_accel_module_list);

/* Crypto keyring */
static TAILQ_HEAD(, spdk_accel_crypto_key) g_keyring = TAILQ_HEAD_INITIALIZER(g_keyring);
static struct spdk_spinlock g_keyring_spin;

/* Global array mapping capabilities to modules */
static struct accel_module g_modules_opc[ACCEL_OPC_LAST] = {};
static char *g_modules_opc_override[ACCEL_OPC_LAST] = {};

static const char *g_opcode_strings[ACCEL_OPC_LAST] = {
	"copy", "fill", "dualcast", "compare", "crc32c", "copy_crc32c",
	"compress", "decompress", "encrypt", "decrypt"
};

enum accel_sequence_state {
	ACCEL_SEQUENCE_STATE_INIT,
	ACCEL_SEQUENCE_STATE_CHECK_VIRTBUF,
	ACCEL_SEQUENCE_STATE_AWAIT_VIRTBUF,
	ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF,
	ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF,
	ACCEL_SEQUENCE_STATE_PULL_DATA,
	ACCEL_SEQUENCE_STATE_AWAIT_PULL_DATA,
	ACCEL_SEQUENCE_STATE_EXEC_TASK,
	ACCEL_SEQUENCE_STATE_AWAIT_TASK,
	ACCEL_SEQUENCE_STATE_COMPLETE_TASK,
	ACCEL_SEQUENCE_STATE_NEXT_TASK,
	ACCEL_SEQUENCE_STATE_PUSH_DATA,
	ACCEL_SEQUENCE_STATE_AWAIT_PUSH_DATA,
	ACCEL_SEQUENCE_STATE_ERROR,
	ACCEL_SEQUENCE_STATE_MAX,
};

static const char *g_seq_states[]
__attribute__((unused)) = {
	[ACCEL_SEQUENCE_STATE_INIT] = "init",
	[ACCEL_SEQUENCE_STATE_CHECK_VIRTBUF] = "check-virtbuf",
	[ACCEL_SEQUENCE_STATE_AWAIT_VIRTBUF] = "await-virtbuf",
	[ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF] = "check-bouncebuf",
	[ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF] = "await-bouncebuf",
	[ACCEL_SEQUENCE_STATE_PULL_DATA] = "pull-data",
	[ACCEL_SEQUENCE_STATE_AWAIT_PULL_DATA] = "await-pull-data",
	[ACCEL_SEQUENCE_STATE_EXEC_TASK] = "exec-task",
	[ACCEL_SEQUENCE_STATE_AWAIT_TASK] = "await-task",
	[ACCEL_SEQUENCE_STATE_COMPLETE_TASK] = "complete-task",
	[ACCEL_SEQUENCE_STATE_NEXT_TASK] = "next-task",
	[ACCEL_SEQUENCE_STATE_PUSH_DATA] = "push-data",
	[ACCEL_SEQUENCE_STATE_AWAIT_PUSH_DATA] = "await-push-data",
	[ACCEL_SEQUENCE_STATE_ERROR] = "error",
	[ACCEL_SEQUENCE_STATE_MAX] = "",
};

#define ACCEL_SEQUENCE_STATE_STRING(s) \
	(((s) >= ACCEL_SEQUENCE_STATE_INIT && (s) < ACCEL_SEQUENCE_STATE_MAX) \
	 ? g_seq_states[s] : "unknown")

struct accel_buffer {
	struct spdk_accel_sequence	*seq;
	void				*buf;
	uint64_t			len;
	struct spdk_iobuf_entry		iobuf;
	TAILQ_ENTRY(accel_buffer)	link;
};

struct accel_io_channel {
	struct spdk_io_channel			*module_ch[ACCEL_OPC_LAST];
	void					*task_pool_base;
	struct spdk_accel_sequence		*seq_pool_base;
	struct accel_buffer			*buf_pool_base;
	TAILQ_HEAD(, spdk_accel_task)		task_pool;
	TAILQ_HEAD(, spdk_accel_sequence)	seq_pool;
	TAILQ_HEAD(, accel_buffer)		buf_pool;
	struct spdk_iobuf_channel		iobuf;
};

TAILQ_HEAD(accel_sequence_tasks, spdk_accel_task);

struct spdk_accel_sequence {
	struct accel_io_channel			*ch;
	struct accel_sequence_tasks		tasks;
	struct accel_sequence_tasks		completed;
	TAILQ_HEAD(, accel_buffer)		bounce_bufs;
	enum accel_sequence_state		state;
	int					status;
	bool					in_process_sequence;
	spdk_accel_completion_cb		cb_fn;
	void					*cb_arg;
	TAILQ_ENTRY(spdk_accel_sequence)	link;
};

static inline void
accel_sequence_set_state(struct spdk_accel_sequence *seq, enum accel_sequence_state state)
{
	SPDK_DEBUGLOG(accel, "seq=%p, setting state: %s -> %s\n", seq,
		      ACCEL_SEQUENCE_STATE_STRING(seq->state), ACCEL_SEQUENCE_STATE_STRING(state));
	seq->state = state;
}

static void
accel_sequence_set_fail(struct spdk_accel_sequence *seq, int status)
{
	accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_ERROR);
	assert(status != 0);
	seq->status = status;
}

int
spdk_accel_get_opc_module_name(enum accel_opcode opcode, const char **module_name)
{
	if (opcode >= ACCEL_OPC_LAST) {
		/* invalid opcode */
		return -EINVAL;
	}

	if (g_modules_opc[opcode].module) {
		*module_name = g_modules_opc[opcode].module->name;
	} else {
		return -ENOENT;
	}

	return 0;
}

void
_accel_for_each_module(struct module_info *info, _accel_for_each_module_fn fn)
{
	struct spdk_accel_module_if *accel_module;
	enum accel_opcode opcode;
	int j = 0;

	TAILQ_FOREACH(accel_module, &spdk_accel_module_list, tailq) {
		for (opcode = 0; opcode < ACCEL_OPC_LAST; opcode++) {
			if (accel_module->supports_opcode(opcode)) {
				info->ops[j] = opcode;
				j++;
			}
		}
		info->name = accel_module->name;
		info->num_ops = j;
		fn(info);
		j = 0;
	}
}

int
_accel_get_opc_name(enum accel_opcode opcode, const char **opcode_name)
{
	int rc = 0;

	if (opcode < ACCEL_OPC_LAST) {
		*opcode_name = g_opcode_strings[opcode];
	} else {
		/* invalid opcode */
		rc = -EINVAL;
	}

	return rc;
}

int
spdk_accel_assign_opc(enum accel_opcode opcode, const char *name)
{
	if (g_modules_started == true) {
		/* we don't allow re-assignment once things have started */
		return -EINVAL;
	}

	if (opcode >= ACCEL_OPC_LAST) {
		/* invalid opcode */
		return -EINVAL;
	}

	/* module selection will be validated after the framework starts. */
	g_modules_opc_override[opcode] = strdup(name);

	return 0;
}

void
spdk_accel_task_complete(struct spdk_accel_task *accel_task, int status)
{
	struct accel_io_channel *accel_ch = accel_task->accel_ch;
	spdk_accel_completion_cb	cb_fn = accel_task->cb_fn;
	void				*cb_arg = accel_task->cb_arg;

	/* We should put the accel_task into the list firstly in order to avoid
	 * the accel task list is exhausted when there is recursive call to
	 * allocate accel_task in user's call back function (cb_fn)
	 */
	TAILQ_INSERT_HEAD(&accel_ch->task_pool, accel_task, link);

	cb_fn(cb_arg, status);
}

inline static struct spdk_accel_task *
_get_task(struct accel_io_channel *accel_ch, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_accel_task *accel_task;

	accel_task = TAILQ_FIRST(&accel_ch->task_pool);
	if (accel_task == NULL) {
		return NULL;
	}

	TAILQ_REMOVE(&accel_ch->task_pool, accel_task, link);
	accel_task->link.tqe_next = NULL;
	accel_task->link.tqe_prev = NULL;

	accel_task->cb_fn = cb_fn;
	accel_task->cb_arg = cb_arg;
	accel_task->accel_ch = accel_ch;
	accel_task->bounce.s.orig_iovs = NULL;
	accel_task->bounce.d.orig_iovs = NULL;

	return accel_task;
}

/* Accel framework public API for copy function */
int
spdk_accel_submit_copy(struct spdk_io_channel *ch, void *dst, void *src,
		       uint64_t nbytes, int flags, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *module = g_modules_opc[ACCEL_OPC_COPY].module;
	struct spdk_io_channel *module_ch = accel_ch->module_ch[ACCEL_OPC_COPY];

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->s.iovs = &accel_task->aux_iovs[SPDK_ACCEL_AUX_IOV_SRC];
	accel_task->d.iovs = &accel_task->aux_iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	accel_task->s.iovs[0].iov_base = src;
	accel_task->s.iovs[0].iov_len = nbytes;
	accel_task->s.iovcnt = 1;
	accel_task->op_code = ACCEL_OPC_COPY;
	accel_task->flags = flags;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;
	accel_task->step_cb_fn = NULL;

	return module->submit_tasks(module_ch, accel_task);
}

/* Accel framework public API for dual cast copy function */
int
spdk_accel_submit_dualcast(struct spdk_io_channel *ch, void *dst1,
			   void *dst2, void *src, uint64_t nbytes, int flags,
			   spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *module = g_modules_opc[ACCEL_OPC_DUALCAST].module;
	struct spdk_io_channel *module_ch = accel_ch->module_ch[ACCEL_OPC_DUALCAST];

	if ((uintptr_t)dst1 & (ALIGN_4K - 1) || (uintptr_t)dst2 & (ALIGN_4K - 1)) {
		SPDK_ERRLOG("Dualcast requires 4K alignment on dst addresses\n");
		return -EINVAL;
	}

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->s.iovs = &accel_task->aux_iovs[SPDK_ACCEL_AUX_IOV_SRC];
	accel_task->d.iovs = &accel_task->aux_iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d2.iovs = &accel_task->aux_iovs[SPDK_ACCEL_AUX_IOV_DST2];
	accel_task->d.iovs[0].iov_base = dst1;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	accel_task->d2.iovs[0].iov_base = dst2;
	accel_task->d2.iovs[0].iov_len = nbytes;
	accel_task->d2.iovcnt = 1;
	accel_task->s.iovs[0].iov_base = src;
	accel_task->s.iovs[0].iov_len = nbytes;
	accel_task->s.iovcnt = 1;
	accel_task->flags = flags;
	accel_task->op_code = ACCEL_OPC_DUALCAST;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;
	accel_task->step_cb_fn = NULL;

	return module->submit_tasks(module_ch, accel_task);
}

/* Accel framework public API for compare function */
int
spdk_accel_submit_compare(struct spdk_io_channel *ch, void *src1,
			  void *src2, uint64_t nbytes, spdk_accel_completion_cb cb_fn,
			  void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *module = g_modules_opc[ACCEL_OPC_COMPARE].module;
	struct spdk_io_channel *module_ch = accel_ch->module_ch[ACCEL_OPC_COMPARE];

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->s.iovs = &accel_task->aux_iovs[SPDK_ACCEL_AUX_IOV_SRC];
	accel_task->s2.iovs = &accel_task->aux_iovs[SPDK_ACCEL_AUX_IOV_SRC2];
	accel_task->s.iovs[0].iov_base = src1;
	accel_task->s.iovs[0].iov_len = nbytes;
	accel_task->s.iovcnt = 1;
	accel_task->s2.iovs[0].iov_base = src2;
	accel_task->s2.iovs[0].iov_len = nbytes;
	accel_task->s2.iovcnt = 1;
	accel_task->op_code = ACCEL_OPC_COMPARE;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;
	accel_task->step_cb_fn = NULL;

	return module->submit_tasks(module_ch, accel_task);
}

/* Accel framework public API for fill function */
int
spdk_accel_submit_fill(struct spdk_io_channel *ch, void *dst,
		       uint8_t fill, uint64_t nbytes, int flags,
		       spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *module = g_modules_opc[ACCEL_OPC_FILL].module;
	struct spdk_io_channel *module_ch = accel_ch->module_ch[ACCEL_OPC_FILL];

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->d.iovs = &accel_task->aux_iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	memset(&accel_task->fill_pattern, fill, sizeof(uint64_t));
	accel_task->flags = flags;
	accel_task->op_code = ACCEL_OPC_FILL;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;
	accel_task->step_cb_fn = NULL;

	return module->submit_tasks(module_ch, accel_task);
}

/* Accel framework public API for CRC-32C function */
int
spdk_accel_submit_crc32c(struct spdk_io_channel *ch, uint32_t *crc_dst,
			 void *src, uint32_t seed, uint64_t nbytes, spdk_accel_completion_cb cb_fn,
			 void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *module = g_modules_opc[ACCEL_OPC_CRC32C].module;
	struct spdk_io_channel *module_ch = accel_ch->module_ch[ACCEL_OPC_CRC32C];

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->s.iovs = &accel_task->aux_iovs[SPDK_ACCEL_AUX_IOV_SRC];
	accel_task->s.iovs[0].iov_base = src;
	accel_task->s.iovs[0].iov_len = nbytes;
	accel_task->s.iovcnt = 1;
	accel_task->crc_dst = crc_dst;
	accel_task->seed = seed;
	accel_task->op_code = ACCEL_OPC_CRC32C;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;
	accel_task->step_cb_fn = NULL;

	return module->submit_tasks(module_ch, accel_task);
}

/* Accel framework public API for chained CRC-32C function */
int
spdk_accel_submit_crc32cv(struct spdk_io_channel *ch, uint32_t *crc_dst,
			  struct iovec *iov, uint32_t iov_cnt, uint32_t seed,
			  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *module = g_modules_opc[ACCEL_OPC_CRC32C].module;
	struct spdk_io_channel *module_ch = accel_ch->module_ch[ACCEL_OPC_CRC32C];

	if (iov == NULL) {
		SPDK_ERRLOG("iov should not be NULL");
		return -EINVAL;
	}

	if (!iov_cnt) {
		SPDK_ERRLOG("iovcnt should not be zero value\n");
		return -EINVAL;
	}

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		SPDK_ERRLOG("no memory\n");
		assert(0);
		return -ENOMEM;
	}

	accel_task->s.iovs = iov;
	accel_task->s.iovcnt = iov_cnt;
	accel_task->crc_dst = crc_dst;
	accel_task->seed = seed;
	accel_task->op_code = ACCEL_OPC_CRC32C;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;
	accel_task->step_cb_fn = NULL;

	return module->submit_tasks(module_ch, accel_task);
}

/* Accel framework public API for copy with CRC-32C function */
int
spdk_accel_submit_copy_crc32c(struct spdk_io_channel *ch, void *dst,
			      void *src, uint32_t *crc_dst, uint32_t seed, uint64_t nbytes,
			      int flags, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *module = g_modules_opc[ACCEL_OPC_COPY_CRC32C].module;
	struct spdk_io_channel *module_ch = accel_ch->module_ch[ACCEL_OPC_COPY_CRC32C];

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->s.iovs = &accel_task->aux_iovs[SPDK_ACCEL_AUX_IOV_SRC];
	accel_task->d.iovs = &accel_task->aux_iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	accel_task->s.iovs[0].iov_base = src;
	accel_task->s.iovs[0].iov_len = nbytes;
	accel_task->s.iovcnt = 1;
	accel_task->crc_dst = crc_dst;
	accel_task->seed = seed;
	accel_task->flags = flags;
	accel_task->op_code = ACCEL_OPC_COPY_CRC32C;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;
	accel_task->step_cb_fn = NULL;

	return module->submit_tasks(module_ch, accel_task);
}

/* Accel framework public API for chained copy + CRC-32C function */
int
spdk_accel_submit_copy_crc32cv(struct spdk_io_channel *ch, void *dst,
			       struct iovec *src_iovs, uint32_t iov_cnt, uint32_t *crc_dst,
			       uint32_t seed, int flags, spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *module = g_modules_opc[ACCEL_OPC_COPY_CRC32C].module;
	struct spdk_io_channel *module_ch = accel_ch->module_ch[ACCEL_OPC_COPY_CRC32C];
	uint64_t nbytes;
	uint32_t i;

	if (src_iovs == NULL) {
		SPDK_ERRLOG("iov should not be NULL");
		return -EINVAL;
	}

	if (!iov_cnt) {
		SPDK_ERRLOG("iovcnt should not be zero value\n");
		return -EINVAL;
	}

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		SPDK_ERRLOG("no memory\n");
		assert(0);
		return -ENOMEM;
	}

	nbytes = 0;
	for (i = 0; i < iov_cnt; i++) {
		nbytes += src_iovs[i].iov_len;
	}

	accel_task->d.iovs = &accel_task->aux_iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = iov_cnt;
	accel_task->crc_dst = crc_dst;
	accel_task->seed = seed;
	accel_task->flags = flags;
	accel_task->op_code = ACCEL_OPC_COPY_CRC32C;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;
	accel_task->step_cb_fn = NULL;

	return module->submit_tasks(module_ch, accel_task);
}

int
spdk_accel_submit_compress(struct spdk_io_channel *ch, void *dst, uint64_t nbytes,
			   struct iovec *src_iovs, size_t src_iovcnt, uint32_t *output_size, int flags,
			   spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *module = g_modules_opc[ACCEL_OPC_COMPRESS].module;
	struct spdk_io_channel *module_ch = accel_ch->module_ch[ACCEL_OPC_COMPRESS];

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->d.iovs = &accel_task->aux_iovs[SPDK_ACCEL_AUX_IOV_DST];
	accel_task->d.iovs[0].iov_base = dst;
	accel_task->d.iovs[0].iov_len = nbytes;
	accel_task->d.iovcnt = 1;
	accel_task->output_size = output_size;
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = src_iovcnt;
	accel_task->flags = flags;
	accel_task->op_code = ACCEL_OPC_COMPRESS;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;
	accel_task->step_cb_fn = NULL;

	return module->submit_tasks(module_ch, accel_task);
}

int
spdk_accel_submit_decompress(struct spdk_io_channel *ch, struct iovec *dst_iovs,
			     size_t dst_iovcnt, struct iovec *src_iovs, size_t src_iovcnt,
			     uint32_t *output_size, int flags, spdk_accel_completion_cb cb_fn,
			     void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *module = g_modules_opc[ACCEL_OPC_DECOMPRESS].module;
	struct spdk_io_channel *module_ch = accel_ch->module_ch[ACCEL_OPC_DECOMPRESS];

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->output_size = output_size;
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = src_iovcnt;
	accel_task->d.iovs = dst_iovs;
	accel_task->d.iovcnt = dst_iovcnt;
	accel_task->flags = flags;
	accel_task->op_code = ACCEL_OPC_DECOMPRESS;
	accel_task->src_domain = NULL;
	accel_task->dst_domain = NULL;
	accel_task->step_cb_fn = NULL;

	return module->submit_tasks(module_ch, accel_task);
}

int
spdk_accel_submit_encrypt(struct spdk_io_channel *ch, struct spdk_accel_crypto_key *key,
			  struct iovec *dst_iovs, uint32_t dst_iovcnt,
			  struct iovec *src_iovs, uint32_t src_iovcnt,
			  uint64_t iv, uint32_t block_size, int flags,
			  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *module = g_modules_opc[ACCEL_OPC_ENCRYPT].module;
	struct spdk_io_channel *module_ch = accel_ch->module_ch[ACCEL_OPC_ENCRYPT];

	if (spdk_unlikely(!dst_iovs || !dst_iovcnt || !src_iovs || !src_iovcnt || !key || !block_size)) {
		return -EINVAL;
	}

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->crypto_key = key;
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = src_iovcnt;
	accel_task->d.iovs = dst_iovs;
	accel_task->d.iovcnt = dst_iovcnt;
	accel_task->iv = iv;
	accel_task->block_size = block_size;
	accel_task->flags = flags;
	accel_task->op_code = ACCEL_OPC_ENCRYPT;

	return module->submit_tasks(module_ch, accel_task);
}

int
spdk_accel_submit_decrypt(struct spdk_io_channel *ch, struct spdk_accel_crypto_key *key,
			  struct iovec *dst_iovs, uint32_t dst_iovcnt,
			  struct iovec *src_iovs, uint32_t src_iovcnt,
			  uint64_t iv, uint32_t block_size, int flags,
			  spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *accel_task;
	struct spdk_accel_module_if *module = g_modules_opc[ACCEL_OPC_DECRYPT].module;
	struct spdk_io_channel *module_ch = accel_ch->module_ch[ACCEL_OPC_DECRYPT];

	if (spdk_unlikely(!dst_iovs || !dst_iovcnt || !src_iovs || !src_iovcnt || !key || !block_size)) {
		return -EINVAL;
	}

	accel_task = _get_task(accel_ch, cb_fn, cb_arg);
	if (accel_task == NULL) {
		return -ENOMEM;
	}

	accel_task->crypto_key = key;
	accel_task->s.iovs = src_iovs;
	accel_task->s.iovcnt = src_iovcnt;
	accel_task->d.iovs = dst_iovs;
	accel_task->d.iovcnt = dst_iovcnt;
	accel_task->iv = iv;
	accel_task->block_size = block_size;
	accel_task->flags = flags;
	accel_task->op_code = ACCEL_OPC_DECRYPT;

	return module->submit_tasks(module_ch, accel_task);
}

static inline struct accel_buffer *
accel_get_buf(struct accel_io_channel *ch, uint64_t len)
{
	struct accel_buffer *buf;

	buf = TAILQ_FIRST(&ch->buf_pool);
	if (spdk_unlikely(buf == NULL)) {
		return NULL;
	}

	TAILQ_REMOVE(&ch->buf_pool, buf, link);
	buf->len = len;
	buf->buf = NULL;
	buf->seq = NULL;

	return buf;
}

static inline void
accel_put_buf(struct accel_io_channel *ch, struct accel_buffer *buf)
{
	if (buf->buf != NULL) {
		spdk_iobuf_put(&ch->iobuf, buf->buf, buf->len);
	}

	TAILQ_INSERT_HEAD(&ch->buf_pool, buf, link);
}

static inline struct spdk_accel_sequence *
accel_sequence_get(struct accel_io_channel *ch)
{
	struct spdk_accel_sequence *seq;

	seq = TAILQ_FIRST(&ch->seq_pool);
	if (seq == NULL) {
		return NULL;
	}

	TAILQ_REMOVE(&ch->seq_pool, seq, link);

	TAILQ_INIT(&seq->tasks);
	TAILQ_INIT(&seq->completed);
	TAILQ_INIT(&seq->bounce_bufs);

	seq->ch = ch;
	seq->status = 0;
	seq->state = ACCEL_SEQUENCE_STATE_INIT;
	seq->in_process_sequence = false;

	return seq;
}

static inline void
accel_sequence_put(struct spdk_accel_sequence *seq)
{
	struct accel_io_channel *ch = seq->ch;
	struct accel_buffer *buf;

	while (!TAILQ_EMPTY(&seq->bounce_bufs)) {
		buf = TAILQ_FIRST(&seq->bounce_bufs);
		TAILQ_REMOVE(&seq->bounce_bufs, buf, link);
		accel_put_buf(seq->ch, buf);
	}

	assert(TAILQ_EMPTY(&seq->tasks));
	assert(TAILQ_EMPTY(&seq->completed));
	seq->ch = NULL;

	TAILQ_INSERT_HEAD(&ch->seq_pool, seq, link);
}

static void accel_sequence_task_cb(void *cb_arg, int status);

static inline struct spdk_accel_task *
accel_sequence_get_task(struct accel_io_channel *ch, struct spdk_accel_sequence *seq,
			spdk_accel_step_cb cb_fn, void *cb_arg)
{
	struct spdk_accel_task *task;

	task = _get_task(ch, accel_sequence_task_cb, seq);
	if (task == NULL) {
		return task;
	}

	task->step_cb_fn = cb_fn;
	task->step_cb_arg = cb_arg;

	return task;
}

int
spdk_accel_append_copy(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
		       struct iovec *dst_iovs, uint32_t dst_iovcnt,
		       struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
		       struct iovec *src_iovs, uint32_t src_iovcnt,
		       struct spdk_memory_domain *src_domain, void *src_domain_ctx,
		       int flags, spdk_accel_step_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	assert(seq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	task->dst_domain = dst_domain;
	task->dst_domain_ctx = dst_domain_ctx;
	task->d.iovs = dst_iovs;
	task->d.iovcnt = dst_iovcnt;
	task->src_domain = src_domain;
	task->src_domain_ctx = src_domain_ctx;
	task->s.iovs = src_iovs;
	task->s.iovcnt = src_iovcnt;
	task->flags = flags;
	task->op_code = ACCEL_OPC_COPY;

	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

int
spdk_accel_append_fill(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
		       void *buf, uint64_t len,
		       struct spdk_memory_domain *domain, void *domain_ctx, uint8_t pattern,
		       int flags, spdk_accel_step_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	assert(seq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	memset(&task->fill_pattern, pattern, sizeof(uint64_t));

	task->d.iovs = &task->aux_iovs[SPDK_ACCEL_AUX_IOV_DST];
	task->d.iovs[0].iov_base = buf;
	task->d.iovs[0].iov_len = len;
	task->d.iovcnt = 1;
	task->src_domain = NULL;
	task->dst_domain = domain;
	task->dst_domain_ctx = domain_ctx;
	task->flags = flags;
	task->op_code = ACCEL_OPC_FILL;

	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

int
spdk_accel_append_decompress(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
			     struct iovec *dst_iovs, size_t dst_iovcnt,
			     struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			     struct iovec *src_iovs, size_t src_iovcnt,
			     struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			     int flags, spdk_accel_step_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	assert(seq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	/* TODO: support output_size for chaining */
	task->output_size = NULL;
	task->dst_domain = dst_domain;
	task->dst_domain_ctx = dst_domain_ctx;
	task->d.iovs = dst_iovs;
	task->d.iovcnt = dst_iovcnt;
	task->src_domain = src_domain;
	task->src_domain_ctx = src_domain_ctx;
	task->s.iovs = src_iovs;
	task->s.iovcnt = src_iovcnt;
	task->flags = flags;
	task->op_code = ACCEL_OPC_DECOMPRESS;

	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

int
spdk_accel_append_encrypt(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
			  struct spdk_accel_crypto_key *key,
			  struct iovec *dst_iovs, uint32_t dst_iovcnt,
			  struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			  struct iovec *src_iovs, uint32_t src_iovcnt,
			  struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			  uint64_t iv, uint32_t block_size, int flags,
			  spdk_accel_step_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	if (spdk_unlikely(!dst_iovs || !dst_iovcnt || !src_iovs || !src_iovcnt || !key ||
			  !block_size)) {
		return -EINVAL;
	}

	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	assert(seq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	task->crypto_key = key;
	task->src_domain = src_domain;
	task->src_domain_ctx = src_domain_ctx;
	task->s.iovs = src_iovs;
	task->s.iovcnt = src_iovcnt;
	task->dst_domain = dst_domain;
	task->dst_domain_ctx = dst_domain_ctx;
	task->d.iovs = dst_iovs;
	task->d.iovcnt = dst_iovcnt;
	task->iv = iv;
	task->block_size = block_size;
	task->flags = flags;
	task->op_code = ACCEL_OPC_ENCRYPT;

	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

int
spdk_accel_append_decrypt(struct spdk_accel_sequence **pseq, struct spdk_io_channel *ch,
			  struct spdk_accel_crypto_key *key,
			  struct iovec *dst_iovs, uint32_t dst_iovcnt,
			  struct spdk_memory_domain *dst_domain, void *dst_domain_ctx,
			  struct iovec *src_iovs, uint32_t src_iovcnt,
			  struct spdk_memory_domain *src_domain, void *src_domain_ctx,
			  uint64_t iv, uint32_t block_size, int flags,
			  spdk_accel_step_cb cb_fn, void *cb_arg)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct spdk_accel_task *task;
	struct spdk_accel_sequence *seq = *pseq;

	if (spdk_unlikely(!dst_iovs || !dst_iovcnt || !src_iovs || !src_iovcnt || !key ||
			  !block_size)) {
		return -EINVAL;
	}

	if (seq == NULL) {
		seq = accel_sequence_get(accel_ch);
		if (spdk_unlikely(seq == NULL)) {
			return -ENOMEM;
		}
	}

	assert(seq->ch == accel_ch);
	task = accel_sequence_get_task(accel_ch, seq, cb_fn, cb_arg);
	if (spdk_unlikely(task == NULL)) {
		if (*pseq == NULL) {
			accel_sequence_put(seq);
		}

		return -ENOMEM;
	}

	task->crypto_key = key;
	task->src_domain = src_domain;
	task->src_domain_ctx = src_domain_ctx;
	task->s.iovs = src_iovs;
	task->s.iovcnt = src_iovcnt;
	task->dst_domain = dst_domain;
	task->dst_domain_ctx = dst_domain_ctx;
	task->d.iovs = dst_iovs;
	task->d.iovcnt = dst_iovcnt;
	task->iv = iv;
	task->block_size = block_size;
	task->flags = flags;
	task->op_code = ACCEL_OPC_DECRYPT;

	TAILQ_INSERT_TAIL(&seq->tasks, task, seq_link);
	*pseq = seq;

	return 0;
}

int
spdk_accel_get_buf(struct spdk_io_channel *ch, uint64_t len, void **buf,
		   struct spdk_memory_domain **domain, void **domain_ctx)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct accel_buffer *accel_buf;

	accel_buf = accel_get_buf(accel_ch, len);
	if (spdk_unlikely(accel_buf == NULL)) {
		return -ENOMEM;
	}

	/* We always return the same pointer and identify the buffers through domain_ctx */
	*buf = ACCEL_BUFFER_BASE;
	*domain_ctx = accel_buf;
	*domain = g_accel_domain;

	return 0;
}

void
spdk_accel_put_buf(struct spdk_io_channel *ch, void *buf,
		   struct spdk_memory_domain *domain, void *domain_ctx)
{
	struct accel_io_channel *accel_ch = spdk_io_channel_get_ctx(ch);
	struct accel_buffer *accel_buf = domain_ctx;

	assert(domain == g_accel_domain);
	assert(buf == ACCEL_BUFFER_BASE);

	accel_put_buf(accel_ch, accel_buf);
}

static void
accel_sequence_complete_tasks(struct spdk_accel_sequence *seq)
{
	struct spdk_accel_task *task;
	struct accel_io_channel *ch = seq->ch;
	spdk_accel_step_cb cb_fn;
	void *cb_arg;

	while (!TAILQ_EMPTY(&seq->completed)) {
		task = TAILQ_FIRST(&seq->completed);
		TAILQ_REMOVE(&seq->completed, task, seq_link);
		cb_fn = task->step_cb_fn;
		cb_arg = task->step_cb_arg;
		TAILQ_INSERT_HEAD(&ch->task_pool, task, link);
		if (cb_fn != NULL) {
			cb_fn(cb_arg);
		}
	}

	while (!TAILQ_EMPTY(&seq->tasks)) {
		task = TAILQ_FIRST(&seq->tasks);
		TAILQ_REMOVE(&seq->tasks, task, seq_link);
		cb_fn = task->step_cb_fn;
		cb_arg = task->step_cb_arg;
		TAILQ_INSERT_HEAD(&ch->task_pool, task, link);
		if (cb_fn != NULL) {
			cb_fn(cb_arg);
		}
	}
}

static void
accel_sequence_complete(struct spdk_accel_sequence *seq)
{
	SPDK_DEBUGLOG(accel, "Completed sequence: %p with status: %d\n", seq, seq->status);

	/* First notify all users that appended operations to this sequence */
	accel_sequence_complete_tasks(seq);

	/* Then notify the user that finished the sequence */
	seq->cb_fn(seq->cb_arg, seq->status);

	accel_sequence_put(seq);
}

static void
accel_update_buf(void **buf, struct accel_buffer *accel_buf)
{
	uintptr_t offset;

	offset = (uintptr_t)(*buf) & ACCEL_BUFFER_OFFSET_MASK;
	assert(offset < accel_buf->len);

	*buf = (char *)accel_buf->buf + offset;
}

static void
accel_update_iovs(struct iovec *iovs, uint32_t iovcnt, struct accel_buffer *buf)
{
	uint32_t i;

	for (i = 0; i < iovcnt; ++i) {
		accel_update_buf(&iovs[i].iov_base, buf);
	}
}

static void
accel_sequence_set_virtbuf(struct spdk_accel_sequence *seq, struct accel_buffer *buf)
{
	struct spdk_accel_task *task;

	/* Now that we've allocated the actual data buffer for this accel_buffer, update all tasks
	 * in a sequence that were using it.
	 */
	TAILQ_FOREACH(task, &seq->tasks, seq_link) {
		if (task->src_domain == g_accel_domain && task->src_domain_ctx == buf) {
			accel_update_iovs(task->s.iovs, task->s.iovcnt, buf);
			task->src_domain = NULL;
		}
		if (task->dst_domain == g_accel_domain && task->dst_domain_ctx == buf) {
			accel_update_iovs(task->d.iovs, task->d.iovcnt, buf);
			task->dst_domain = NULL;
		}
	}
}

static void accel_process_sequence(struct spdk_accel_sequence *seq);

static void
accel_iobuf_get_virtbuf_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct accel_buffer *accel_buf;

	accel_buf = SPDK_CONTAINEROF(entry, struct accel_buffer, iobuf);

	assert(accel_buf->seq != NULL);
	assert(accel_buf->buf == NULL);
	accel_buf->buf = buf;

	assert(accel_buf->seq->state == ACCEL_SEQUENCE_STATE_AWAIT_VIRTBUF);
	accel_sequence_set_state(accel_buf->seq, ACCEL_SEQUENCE_STATE_CHECK_VIRTBUF);
	accel_sequence_set_virtbuf(accel_buf->seq, accel_buf);
	accel_process_sequence(accel_buf->seq);
}

static bool
accel_sequence_alloc_buf(struct spdk_accel_sequence *seq, struct accel_buffer *buf,
			 spdk_iobuf_get_cb cb_fn)
{
	struct accel_io_channel *ch = seq->ch;

	assert(buf->buf == NULL);
	assert(buf->seq == NULL);

	buf->seq = seq;
	buf->buf = spdk_iobuf_get(&ch->iobuf, buf->len, &buf->iobuf, cb_fn);
	if (buf->buf == NULL) {
		return false;
	}

	return true;
}

static bool
accel_sequence_check_virtbuf(struct spdk_accel_sequence *seq, struct spdk_accel_task *task)
{
	/* If a task doesn't have dst/src (e.g. fill, crc32), its dst/src domain should be set to
	 * NULL */
	if (task->src_domain == g_accel_domain) {
		if (!accel_sequence_alloc_buf(seq, task->src_domain_ctx,
					      accel_iobuf_get_virtbuf_cb)) {
			return false;
		}

		accel_sequence_set_virtbuf(seq, task->src_domain_ctx);
	}

	if (task->dst_domain == g_accel_domain) {
		if (!accel_sequence_alloc_buf(seq, task->dst_domain_ctx,
					      accel_iobuf_get_virtbuf_cb)) {
			return false;
		}

		accel_sequence_set_virtbuf(seq, task->dst_domain_ctx);
	}

	return true;
}

static inline uint64_t
accel_get_iovlen(struct iovec *iovs, uint32_t iovcnt)
{
	uint64_t result = 0;
	uint32_t i;

	for (i = 0; i < iovcnt; ++i) {
		result += iovs[i].iov_len;
	}

	return result;
}

static inline void
accel_set_bounce_buffer(struct spdk_accel_bounce_buffer *bounce, struct iovec **iovs,
			uint32_t *iovcnt, struct spdk_memory_domain **domain, void **domain_ctx,
			struct accel_buffer *buf)
{
	bounce->orig_iovs = *iovs;
	bounce->orig_iovcnt = *iovcnt;
	bounce->orig_domain = *domain;
	bounce->orig_domain_ctx = *domain_ctx;
	bounce->iov.iov_base = buf->buf;
	bounce->iov.iov_len = buf->len;

	*iovs = &bounce->iov;
	*iovcnt = 1;
	*domain = NULL;
}

static void
accel_iobuf_get_src_bounce_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct spdk_accel_task *task;
	struct accel_buffer *accel_buf;

	accel_buf = SPDK_CONTAINEROF(entry, struct accel_buffer, iobuf);
	assert(accel_buf->buf == NULL);
	accel_buf->buf = buf;

	task = TAILQ_FIRST(&accel_buf->seq->tasks);
	assert(task != NULL);

	assert(accel_buf->seq->state == ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF);
	accel_sequence_set_state(accel_buf->seq, ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF);
	accel_set_bounce_buffer(&task->bounce.s, &task->s.iovs, &task->s.iovcnt, &task->src_domain,
				&task->src_domain_ctx, accel_buf);
	accel_process_sequence(accel_buf->seq);
}

static void
accel_iobuf_get_dst_bounce_cb(struct spdk_iobuf_entry *entry, void *buf)
{
	struct spdk_accel_task *task;
	struct accel_buffer *accel_buf;

	accel_buf = SPDK_CONTAINEROF(entry, struct accel_buffer, iobuf);
	assert(accel_buf->buf == NULL);
	accel_buf->buf = buf;

	task = TAILQ_FIRST(&accel_buf->seq->tasks);
	assert(task != NULL);

	assert(accel_buf->seq->state == ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF);
	accel_sequence_set_state(accel_buf->seq, ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF);
	accel_set_bounce_buffer(&task->bounce.d, &task->d.iovs, &task->d.iovcnt, &task->dst_domain,
				&task->dst_domain_ctx, accel_buf);
	accel_process_sequence(accel_buf->seq);
}

static int
accel_sequence_check_bouncebuf(struct spdk_accel_sequence *seq, struct spdk_accel_task *task)
{
	struct accel_buffer *buf;

	if (task->src_domain != NULL) {
		/* By the time we're here, accel buffers should have been allocated */
		assert(task->src_domain != g_accel_domain);

		buf = accel_get_buf(seq->ch, accel_get_iovlen(task->s.iovs, task->s.iovcnt));
		if (buf == NULL) {
			SPDK_ERRLOG("Couldn't allocate buffer descriptor\n");
			return -ENOMEM;
		}

		TAILQ_INSERT_TAIL(&seq->bounce_bufs, buf, link);
		if (!accel_sequence_alloc_buf(seq, buf, accel_iobuf_get_src_bounce_cb)) {
			return -EAGAIN;
		}

		accel_set_bounce_buffer(&task->bounce.s, &task->s.iovs, &task->s.iovcnt,
					&task->src_domain, &task->src_domain_ctx, buf);
	}

	if (task->dst_domain != NULL) {
		/* By the time we're here, accel buffers should have been allocated */
		assert(task->dst_domain != g_accel_domain);

		buf = accel_get_buf(seq->ch, accel_get_iovlen(task->d.iovs, task->d.iovcnt));
		if (buf == NULL) {
			/* The src buffer will be released when a sequence is completed */
			SPDK_ERRLOG("Couldn't allocate buffer descriptor\n");
			return -ENOMEM;
		}

		TAILQ_INSERT_TAIL(&seq->bounce_bufs, buf, link);
		if (!accel_sequence_alloc_buf(seq, buf, accel_iobuf_get_dst_bounce_cb)) {
			return -EAGAIN;
		}

		accel_set_bounce_buffer(&task->bounce.d, &task->d.iovs, &task->d.iovcnt,
					&task->dst_domain, &task->dst_domain_ctx, buf);
	}

	return 0;
}

static void
accel_task_pull_data_cb(void *ctx, int status)
{
	struct spdk_accel_sequence *seq = ctx;

	assert(seq->state == ACCEL_SEQUENCE_STATE_AWAIT_PULL_DATA);
	if (spdk_likely(status == 0)) {
		accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_EXEC_TASK);
	} else {
		accel_sequence_set_fail(seq, status);
	}

	accel_process_sequence(seq);
}

static void
accel_task_pull_data(struct spdk_accel_sequence *seq, struct spdk_accel_task *task)
{
	int rc;

	assert(task->bounce.s.orig_iovs != NULL);
	assert(task->bounce.s.orig_domain != NULL);
	assert(task->bounce.s.orig_domain != g_accel_domain);
	assert(!g_modules_opc[task->op_code].supports_memory_domains);

	rc = spdk_memory_domain_pull_data(task->bounce.s.orig_domain,
					  task->bounce.s.orig_domain_ctx,
					  task->bounce.s.orig_iovs, task->bounce.s.orig_iovcnt,
					  task->s.iovs, task->s.iovcnt,
					  accel_task_pull_data_cb, seq);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to pull data from memory domain: %s, rc: %d\n",
			    spdk_memory_domain_get_dma_device_id(task->bounce.s.orig_domain), rc);
		accel_sequence_set_fail(seq, rc);
	}
}

static void
accel_task_push_data_cb(void *ctx, int status)
{
	struct spdk_accel_sequence *seq = ctx;

	assert(seq->state == ACCEL_SEQUENCE_STATE_AWAIT_PUSH_DATA);
	if (spdk_likely(status == 0)) {
		accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_NEXT_TASK);
	} else {
		accel_sequence_set_fail(seq, status);
	}

	accel_process_sequence(seq);
}

static void
accel_task_push_data(struct spdk_accel_sequence *seq, struct spdk_accel_task *task)
{
	int rc;

	assert(task->bounce.d.orig_iovs != NULL);
	assert(task->bounce.d.orig_domain != NULL);
	assert(task->bounce.d.orig_domain != g_accel_domain);
	assert(!g_modules_opc[task->op_code].supports_memory_domains);

	rc = spdk_memory_domain_push_data(task->bounce.d.orig_domain,
					  task->bounce.d.orig_domain_ctx,
					  task->bounce.d.orig_iovs, task->bounce.d.orig_iovcnt,
					  task->d.iovs, task->d.iovcnt,
					  accel_task_push_data_cb, seq);
	if (spdk_unlikely(rc != 0)) {
		SPDK_ERRLOG("Failed to push data to memory domain: %s, rc: %d\n",
			    spdk_memory_domain_get_dma_device_id(task->bounce.s.orig_domain), rc);
		accel_sequence_set_fail(seq, rc);
	}
}

static void
accel_process_sequence(struct spdk_accel_sequence *seq)
{
	struct accel_io_channel *accel_ch = seq->ch;
	struct spdk_accel_module_if *module;
	struct spdk_io_channel *module_ch;
	struct spdk_accel_task *task;
	enum accel_sequence_state state;
	int rc;

	/* Prevent recursive calls to this function */
	if (spdk_unlikely(seq->in_process_sequence)) {
		return;
	}
	seq->in_process_sequence = true;

	task = TAILQ_FIRST(&seq->tasks);
	assert(task != NULL);

	do {
		state = seq->state;
		switch (state) {
		case ACCEL_SEQUENCE_STATE_INIT:
		case ACCEL_SEQUENCE_STATE_CHECK_VIRTBUF:
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_AWAIT_VIRTBUF);
			if (!accel_sequence_check_virtbuf(seq, task)) {
				/* We couldn't allocate a buffer, wait until one is available */
				break;
			}
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF);
		/* Fall through */
		case ACCEL_SEQUENCE_STATE_CHECK_BOUNCEBUF:
			/* If a module supports memory domains, we don't need to allocate bounce
			 * buffers */
			if (g_modules_opc[task->op_code].supports_memory_domains) {
				accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_EXEC_TASK);
				break;
			}
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF);
			rc = accel_sequence_check_bouncebuf(seq, task);
			if (rc != 0) {
				/* We couldn't allocate a buffer, wait until one is available */
				if (rc == -EAGAIN) {
					break;
				}
				accel_sequence_set_fail(seq, rc);
				break;
			}
			if (task->bounce.s.orig_iovs != NULL) {
				accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_PULL_DATA);
				break;
			}
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_EXEC_TASK);
		/* Fall through */
		case ACCEL_SEQUENCE_STATE_EXEC_TASK:
			SPDK_DEBUGLOG(accel, "Executing %s operation, sequence: %p\n",
				      g_opcode_strings[task->op_code], seq);

			module = g_modules_opc[task->op_code].module;
			module_ch = accel_ch->module_ch[task->op_code];

			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_AWAIT_TASK);
			rc = module->submit_tasks(module_ch, task);
			if (spdk_unlikely(rc != 0)) {
				SPDK_ERRLOG("Failed to submit %s operation, sequence: %p\n",
					    g_opcode_strings[task->op_code], seq);
				accel_sequence_set_fail(seq, rc);
			}
			break;
		case ACCEL_SEQUENCE_STATE_PULL_DATA:
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_AWAIT_PULL_DATA);
			accel_task_pull_data(seq, task);
			break;
		case ACCEL_SEQUENCE_STATE_COMPLETE_TASK:
			if (task->bounce.d.orig_iovs != NULL) {
				accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_PUSH_DATA);
				break;
			}
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_NEXT_TASK);
			break;
		case ACCEL_SEQUENCE_STATE_PUSH_DATA:
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_AWAIT_PUSH_DATA);
			accel_task_push_data(seq, task);
			break;
		case ACCEL_SEQUENCE_STATE_NEXT_TASK:
			TAILQ_REMOVE(&seq->tasks, task, seq_link);
			TAILQ_INSERT_TAIL(&seq->completed, task, seq_link);
			/* Check if there are any remaining tasks */
			task = TAILQ_FIRST(&seq->tasks);
			if (task == NULL) {
				/* Immediately return here to make sure we don't touch the sequence
				 * after it's completed */
				accel_sequence_complete(seq);
				return;
			}
			accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_INIT);
			break;
		case ACCEL_SEQUENCE_STATE_ERROR:
			/* Immediately return here to make sure we don't touch the sequence
			 * after it's completed */
			assert(seq->status != 0);
			accel_sequence_complete(seq);
			return;
		case ACCEL_SEQUENCE_STATE_AWAIT_VIRTBUF:
		case ACCEL_SEQUENCE_STATE_AWAIT_BOUNCEBUF:
		case ACCEL_SEQUENCE_STATE_AWAIT_PULL_DATA:
		case ACCEL_SEQUENCE_STATE_AWAIT_TASK:
		case ACCEL_SEQUENCE_STATE_AWAIT_PUSH_DATA:
			break;
		default:
			assert(0 && "bad state");
			break;
		}
	} while (seq->state != state);

	seq->in_process_sequence = false;
}

static void
accel_sequence_task_cb(void *cb_arg, int status)
{
	struct spdk_accel_sequence *seq = cb_arg;
	struct spdk_accel_task *task = TAILQ_FIRST(&seq->tasks);
	struct accel_io_channel *accel_ch = seq->ch;

	/* spdk_accel_task_complete() puts the task back to the task pool, but we don't want to do
	 * that if a task is part of a sequence.  Removing the task from that pool here is the
	 * easiest way to prevent this, even though it is a bit hacky.
	 */
	assert(task != NULL);
	TAILQ_REMOVE(&accel_ch->task_pool, task, link);

	assert(seq->state == ACCEL_SEQUENCE_STATE_AWAIT_TASK);
	accel_sequence_set_state(seq, ACCEL_SEQUENCE_STATE_COMPLETE_TASK);

	if (spdk_unlikely(status != 0)) {
		SPDK_ERRLOG("Failed to execute %s operation, sequence: %p\n",
			    g_opcode_strings[task->op_code], seq);
		accel_sequence_set_fail(seq, status);
	}

	accel_process_sequence(seq);
}

static bool
accel_compare_iovs(struct iovec *iova, uint32_t iovacnt, struct iovec *iovb, uint32_t iovbcnt)
{
	/* For now, just do a dumb check that the iovecs arrays are exactly the same */
	if (iovacnt != iovbcnt) {
		return false;
	}

	return memcmp(iova, iovb, sizeof(*iova) * iovacnt) == 0;
}

static void
accel_sequence_merge_tasks(struct spdk_accel_sequence *seq, struct spdk_accel_task *task,
			   struct spdk_accel_task **next_task)
{
	struct spdk_accel_task *next = *next_task;

	switch (task->op_code) {
	case ACCEL_OPC_COPY:
		/* We only allow changing src of operations that actually have a src, e.g. we never
		 * do it for fill.  Theoretically, it is possible, but we'd have to be careful to
		 * change the src of the operation after fill (which in turn could also be a fill).
		 * So, for the sake of simplicity, skip this type of operations for now.
		 */
		if (next->op_code != ACCEL_OPC_DECOMPRESS &&
		    next->op_code != ACCEL_OPC_COPY &&
		    next->op_code != ACCEL_OPC_ENCRYPT &&
		    next->op_code != ACCEL_OPC_DECRYPT) {
			break;
		}
		if (task->dst_domain != next->src_domain) {
			break;
		}
		if (!accel_compare_iovs(task->d.iovs, task->d.iovcnt,
					next->s.iovs, next->s.iovcnt)) {
			break;
		}
		next->s.iovs = task->s.iovs;
		next->s.iovcnt = task->s.iovcnt;
		next->src_domain = task->src_domain;
		next->src_domain_ctx = task->src_domain_ctx;
		TAILQ_REMOVE(&seq->tasks, task, seq_link);
		TAILQ_INSERT_TAIL(&seq->completed, task, seq_link);
		break;
	case ACCEL_OPC_DECOMPRESS:
	case ACCEL_OPC_FILL:
	case ACCEL_OPC_ENCRYPT:
	case ACCEL_OPC_DECRYPT:
		/* We can only merge tasks when one of them is a copy */
		if (next->op_code != ACCEL_OPC_COPY) {
			break;
		}
		if (task->dst_domain != next->src_domain) {
			break;
		}
		if (!accel_compare_iovs(task->d.iovs, task->d.iovcnt,
					next->s.iovs, next->s.iovcnt)) {
			break;
		}
		task->d.iovs = next->d.iovs;
		task->d.iovcnt = next->d.iovcnt;
		task->dst_domain = next->dst_domain;
		task->dst_domain_ctx = next->dst_domain_ctx;
		/* We're removing next_task from the tasks queue, so we need to update its pointer,
		 * so that the TAILQ_FOREACH_SAFE() loop below works correctly */
		*next_task = TAILQ_NEXT(next, seq_link);
		TAILQ_REMOVE(&seq->tasks, next, seq_link);
		TAILQ_INSERT_TAIL(&seq->completed, next, seq_link);
		break;
	default:
		assert(0 && "bad opcode");
		break;
	}
}

int
spdk_accel_sequence_finish(struct spdk_accel_sequence *seq,
			   spdk_accel_completion_cb cb_fn, void *cb_arg)
{
	struct spdk_accel_task *task, *next;

	/* Try to remove any copy operations if possible */
	TAILQ_FOREACH_SAFE(task, &seq->tasks, seq_link, next) {
		if (next == NULL) {
			break;
		}
		accel_sequence_merge_tasks(seq, task, &next);
	}

	seq->cb_fn = cb_fn;
	seq->cb_arg = cb_arg;

	accel_process_sequence(seq);

	return 0;
}

void
spdk_accel_sequence_reverse(struct spdk_accel_sequence *seq)
{
	struct accel_sequence_tasks tasks = TAILQ_HEAD_INITIALIZER(tasks);
	struct spdk_accel_task *task;

	assert(TAILQ_EMPTY(&seq->completed));
	TAILQ_SWAP(&tasks, &seq->tasks, spdk_accel_task, seq_link);

	while (!TAILQ_EMPTY(&tasks)) {
		task = TAILQ_FIRST(&tasks);
		TAILQ_REMOVE(&tasks, task, seq_link);
		TAILQ_INSERT_HEAD(&seq->tasks, task, seq_link);
	}
}

void
spdk_accel_sequence_abort(struct spdk_accel_sequence *seq)
{
	if (seq == NULL) {
		return;
	}

	accel_sequence_complete_tasks(seq);
	accel_sequence_put(seq);
}

static struct spdk_accel_module_if *
_module_find_by_name(const char *name)
{
	struct spdk_accel_module_if *accel_module = NULL;

	TAILQ_FOREACH(accel_module, &spdk_accel_module_list, tailq) {
		if (strcmp(name, accel_module->name) == 0) {
			break;
		}
	}

	return accel_module;
}

static inline struct spdk_accel_crypto_key *
_accel_crypto_key_get(const char *name)
{
	struct spdk_accel_crypto_key *key;

	assert(spdk_spin_held(&g_keyring_spin));

	TAILQ_FOREACH(key, &g_keyring, link) {
		if (strcmp(name, key->param.key_name) == 0) {
			return key;
		}
	}

	return NULL;
}

static void
accel_crypto_key_free_mem(struct spdk_accel_crypto_key *key)
{
	if (key->param.hex_key) {
		spdk_memset_s(key->param.hex_key, key->key_size * 2, 0, key->key_size * 2);
		free(key->param.hex_key);
	}
	if (key->param.hex_key2) {
		spdk_memset_s(key->param.hex_key2, key->key2_size * 2, 0, key->key2_size * 2);
		free(key->param.hex_key2);
	}
	free(key->param.key_name);
	free(key->param.cipher);
	if (key->key) {
		spdk_memset_s(key->key, key->key_size, 0, key->key_size);
		free(key->key);
	}
	if (key->key2) {
		spdk_memset_s(key->key2, key->key2_size, 0, key->key2_size);
		free(key->key2);
	}
	free(key);
}

static void
accel_crypto_key_destroy_unsafe(struct spdk_accel_crypto_key *key)
{
	assert(key->module_if);
	assert(key->module_if->crypto_key_deinit);

	key->module_if->crypto_key_deinit(key);
	accel_crypto_key_free_mem(key);
}

/*
 * This function mitigates a timing side channel which could be caused by using strcmp()
 * Please refer to chapter "Mitigating Information Leakage Based on Variable Timing" in
 * the article [1] for more details
 * [1] https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/secure-coding/mitigate-timing-side-channel-crypto-implementation.html
 */
static bool
accel_aes_xts_keys_equal(const char *k1, size_t k1_len, const char *k2, size_t k2_len)
{
	size_t i;
	volatile size_t x = k1_len ^ k2_len;

	for (i = 0; ((i < k1_len) & (i < k2_len)); i++) {
		x |= k1[i] ^ k2[i];
	}

	return x == 0;
}

int
spdk_accel_crypto_key_create(const struct spdk_accel_crypto_key_create_param *param)
{
	struct spdk_accel_module_if *module;
	struct spdk_accel_crypto_key *key;
	size_t hex_key_size, hex_key2_size;
	int rc;

	if (!param || !param->hex_key || !param->cipher || !param->key_name) {
		return -EINVAL;
	}

	if (g_modules_opc[ACCEL_OPC_ENCRYPT].module != g_modules_opc[ACCEL_OPC_DECRYPT].module) {
		/* hardly ever possible, but let's check and warn the user */
		SPDK_ERRLOG("Different accel modules are used for encryption and decryption\n");
	}
	module = g_modules_opc[ACCEL_OPC_ENCRYPT].module;

	if (!module) {
		SPDK_ERRLOG("No accel module found assigned for crypto operation\n");
		return -ENOENT;
	}
	if (!module->crypto_key_init) {
		SPDK_ERRLOG("Accel module \"%s\" doesn't support crypto operations\n", module->name);
		return -ENOTSUP;
	}

	key = calloc(1, sizeof(*key));
	if (!key) {
		return -ENOMEM;
	}

	key->param.key_name = strdup(param->key_name);
	if (!key->param.key_name) {
		rc = -ENOMEM;
		goto error;
	}

	key->param.cipher = strdup(param->cipher);
	if (!key->param.cipher) {
		rc = -ENOMEM;
		goto error;
	}

	hex_key_size = strnlen(param->hex_key, SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH);
	if (hex_key_size == SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH) {
		SPDK_ERRLOG("key1 size exceeds max %d\n", SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH);
		rc = -EINVAL;
		goto error;
	}
	key->param.hex_key = strdup(param->hex_key);
	if (!key->param.hex_key) {
		rc = -ENOMEM;
		goto error;
	}

	key->key_size = hex_key_size / 2;
	key->key = spdk_unhexlify(key->param.hex_key);
	if (!key->key) {
		SPDK_ERRLOG("Failed to unhexlify key1\n");
		rc = -EINVAL;
		goto error;
	}

	if (param->hex_key2) {
		hex_key2_size = strnlen(param->hex_key2, SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH);
		if (hex_key2_size == SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH) {
			SPDK_ERRLOG("key2 size exceeds max %d\n", SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH);
			rc = -EINVAL;
			goto error;
		}
		key->param.hex_key2 = strdup(param->hex_key2);
		if (!key->param.hex_key2) {
			rc = -ENOMEM;
			goto error;
		}

		key->key2_size = hex_key2_size / 2;
		key->key2 = spdk_unhexlify(key->param.hex_key2);
		if (!key->key2) {
			SPDK_ERRLOG("Failed to unhexlify key2\n");
			rc = -EINVAL;
			goto error;
		}

		if (accel_aes_xts_keys_equal(key->key, key->key_size, key->key2, key->key2_size)) {
			SPDK_ERRLOG("Identical keys are not secure\n");
			rc = -EINVAL;
			goto error;
		}
	}

	key->module_if = module;

	spdk_spin_lock(&g_keyring_spin);
	if (_accel_crypto_key_get(param->key_name)) {
		rc = -EEXIST;
	} else {
		rc = module->crypto_key_init(key);
		if (!rc) {
			TAILQ_INSERT_TAIL(&g_keyring, key, link);
		}
	}
	spdk_spin_unlock(&g_keyring_spin);

	if (rc) {
		goto error;
	}

	return 0;

error:
	accel_crypto_key_free_mem(key);
	return rc;
}

int
spdk_accel_crypto_key_destroy(struct spdk_accel_crypto_key *key)
{
	if (!key || !key->module_if) {
		return -EINVAL;
	}

	spdk_spin_lock(&g_keyring_spin);
	if (!_accel_crypto_key_get(key->param.key_name)) {
		spdk_spin_unlock(&g_keyring_spin);
		return -ENOENT;
	}
	TAILQ_REMOVE(&g_keyring, key, link);
	spdk_spin_unlock(&g_keyring_spin);

	accel_crypto_key_destroy_unsafe(key);

	return 0;
}

struct spdk_accel_crypto_key *
spdk_accel_crypto_key_get(const char *name)
{
	struct spdk_accel_crypto_key *key;

	spdk_spin_lock(&g_keyring_spin);
	key = _accel_crypto_key_get(name);
	spdk_spin_unlock(&g_keyring_spin);

	return key;
}

/* Helper function when accel modules register with the framework. */
void
spdk_accel_module_list_add(struct spdk_accel_module_if *accel_module)
{
	if (_module_find_by_name(accel_module->name)) {
		SPDK_NOTICELOG("Accel module %s already registered\n", accel_module->name);
		assert(false);
		return;
	}

	/* Make sure that the software module is at the head of the list, this
	 * will assure that all opcodes are later assigned to software first and
	 * then updated to HW modules as they are registered.
	 */
	if (strcmp(accel_module->name, "software") == 0) {
		TAILQ_INSERT_HEAD(&spdk_accel_module_list, accel_module, tailq);
	} else {
		TAILQ_INSERT_TAIL(&spdk_accel_module_list, accel_module, tailq);
	}

	if (accel_module->get_ctx_size && accel_module->get_ctx_size() > g_max_accel_module_size) {
		g_max_accel_module_size = accel_module->get_ctx_size();
	}
}

/* Framework level channel create callback. */
static int
accel_create_channel(void *io_device, void *ctx_buf)
{
	struct accel_io_channel	*accel_ch = ctx_buf;
	struct spdk_accel_task *accel_task;
	struct spdk_accel_sequence *seq;
	struct accel_buffer *buf;
	uint8_t *task_mem;
	int i = 0, j, rc;

	accel_ch->task_pool_base = calloc(MAX_TASKS_PER_CHANNEL, g_max_accel_module_size);
	if (accel_ch->task_pool_base == NULL) {
		return -ENOMEM;
	}

	accel_ch->seq_pool_base = calloc(MAX_TASKS_PER_CHANNEL, sizeof(struct spdk_accel_sequence));
	if (accel_ch->seq_pool_base == NULL) {
		goto err;
	}

	accel_ch->buf_pool_base = calloc(MAX_TASKS_PER_CHANNEL, sizeof(struct accel_buffer));
	if (accel_ch->buf_pool_base == NULL) {
		goto err;
	}

	TAILQ_INIT(&accel_ch->task_pool);
	TAILQ_INIT(&accel_ch->seq_pool);
	TAILQ_INIT(&accel_ch->buf_pool);
	task_mem = accel_ch->task_pool_base;
	for (i = 0 ; i < MAX_TASKS_PER_CHANNEL; i++) {
		accel_task = (struct spdk_accel_task *)task_mem;
		seq = &accel_ch->seq_pool_base[i];
		buf = &accel_ch->buf_pool_base[i];
		TAILQ_INSERT_TAIL(&accel_ch->task_pool, accel_task, link);
		TAILQ_INSERT_TAIL(&accel_ch->seq_pool, seq, link);
		TAILQ_INSERT_TAIL(&accel_ch->buf_pool, buf, link);
		task_mem += g_max_accel_module_size;
	}

	/* Assign modules and get IO channels for each */
	for (i = 0; i < ACCEL_OPC_LAST; i++) {
		accel_ch->module_ch[i] = g_modules_opc[i].module->get_io_channel();
		/* This can happen if idxd runs out of channels. */
		if (accel_ch->module_ch[i] == NULL) {
			goto err;
		}
	}

	rc = spdk_iobuf_channel_init(&accel_ch->iobuf, "accel", ACCEL_SMALL_CACHE_SIZE,
				     ACCEL_LARGE_CACHE_SIZE);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to initialize iobuf accel channel\n");
		goto err;
	}

	return 0;
err:
	for (j = 0; j < i; j++) {
		spdk_put_io_channel(accel_ch->module_ch[j]);
	}
	free(accel_ch->task_pool_base);
	free(accel_ch->seq_pool_base);
	free(accel_ch->buf_pool_base);
	return -ENOMEM;
}

/* Framework level channel destroy callback. */
static void
accel_destroy_channel(void *io_device, void *ctx_buf)
{
	struct accel_io_channel	*accel_ch = ctx_buf;
	int i;

	spdk_iobuf_channel_fini(&accel_ch->iobuf);

	for (i = 0; i < ACCEL_OPC_LAST; i++) {
		assert(accel_ch->module_ch[i] != NULL);
		spdk_put_io_channel(accel_ch->module_ch[i]);
		accel_ch->module_ch[i] = NULL;
	}

	free(accel_ch->task_pool_base);
	free(accel_ch->seq_pool_base);
	free(accel_ch->buf_pool_base);
}

struct spdk_io_channel *
spdk_accel_get_io_channel(void)
{
	return spdk_get_io_channel(&spdk_accel_module_list);
}

static void
accel_module_initialize(void)
{
	struct spdk_accel_module_if *accel_module;

	TAILQ_FOREACH(accel_module, &spdk_accel_module_list, tailq) {
		accel_module->module_init();
	}
}

static void
accel_module_init_opcode(enum accel_opcode opcode)
{
	struct accel_module *module = &g_modules_opc[opcode];
	struct spdk_accel_module_if *module_if = module->module;

	if (module_if->get_memory_domains != NULL) {
		module->supports_memory_domains = module_if->get_memory_domains(NULL, 0) > 0;
	}
}

int
spdk_accel_initialize(void)
{
	enum accel_opcode op;
	struct spdk_accel_module_if *accel_module = NULL;
	int rc;

	rc = spdk_memory_domain_create(&g_accel_domain, SPDK_DMA_DEVICE_TYPE_ACCEL, NULL,
				       "SPDK_ACCEL_DMA_DEVICE");
	if (rc != 0) {
		SPDK_ERRLOG("Failed to create accel memory domain\n");
		return rc;
	}

	spdk_spin_init(&g_keyring_spin);

	g_modules_started = true;
	accel_module_initialize();

	/* Create our priority global map of opcodes to modules, we populate starting
	 * with the software module (guaranteed to be first on the list) and then
	 * updating opcodes with HW modules that have been initialized.
	 * NOTE: all opcodes must be supported by software in the event that no HW
	 * modules are initialized to support the operation.
	 */
	TAILQ_FOREACH(accel_module, &spdk_accel_module_list, tailq) {
		for (op = 0; op < ACCEL_OPC_LAST; op++) {
			if (accel_module->supports_opcode(op)) {
				g_modules_opc[op].module = accel_module;
				SPDK_DEBUGLOG(accel, "OPC 0x%x now assigned to %s\n", op, accel_module->name);
			}
		}
	}

	/* Now lets check for overrides and apply all that exist */
	for (op = 0; op < ACCEL_OPC_LAST; op++) {
		if (g_modules_opc_override[op] != NULL) {
			accel_module = _module_find_by_name(g_modules_opc_override[op]);
			if (accel_module == NULL) {
				SPDK_ERRLOG("Invalid module name of %s\n", g_modules_opc_override[op]);
				rc = -EINVAL;
				goto error;
			}
			if (accel_module->supports_opcode(op) == false) {
				SPDK_ERRLOG("Module %s does not support op code %d\n", accel_module->name, op);
				rc = -EINVAL;
				goto error;
			}
			g_modules_opc[op].module = accel_module;
		}
	}

	if (g_modules_opc[ACCEL_OPC_ENCRYPT].module != g_modules_opc[ACCEL_OPC_DECRYPT].module) {
		SPDK_ERRLOG("Different accel modules are assigned to encrypt and decrypt operations");
		rc = -EINVAL;
		goto error;
	}

	for (op = 0; op < ACCEL_OPC_LAST; op++) {
		assert(g_modules_opc[op].module != NULL);
		accel_module_init_opcode(op);
	}

	rc = spdk_iobuf_register_module("accel");
	if (rc != 0) {
		SPDK_ERRLOG("Failed to register accel iobuf module\n");
		goto error;
	}

	/*
	 * We need a unique identifier for the accel framework, so use the
	 * spdk_accel_module_list address for this purpose.
	 */
	spdk_io_device_register(&spdk_accel_module_list, accel_create_channel, accel_destroy_channel,
				sizeof(struct accel_io_channel), "accel");

	return 0;
error:
	spdk_memory_domain_destroy(g_accel_domain);

	return rc;
}

static void
accel_module_finish_cb(void)
{
	spdk_accel_fini_cb cb_fn = g_fini_cb_fn;

	spdk_memory_domain_destroy(g_accel_domain);

	cb_fn(g_fini_cb_arg);
	g_fini_cb_fn = NULL;
	g_fini_cb_arg = NULL;
}

static void
accel_write_overridden_opc(struct spdk_json_write_ctx *w, const char *opc_str,
			   const char *module_str)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "accel_assign_opc");
	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "opname", opc_str);
	spdk_json_write_named_string(w, "module", module_str);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static void
__accel_crypto_key_dump_param(struct spdk_json_write_ctx *w, struct spdk_accel_crypto_key *key)
{
	spdk_json_write_named_string(w, "name", key->param.key_name);
	spdk_json_write_named_string(w, "cipher", key->param.cipher);
	spdk_json_write_named_string(w, "key", key->param.hex_key);
	if (key->param.hex_key2) {
		spdk_json_write_named_string(w, "key2", key->param.hex_key2);
	}
}

void
_accel_crypto_key_dump_param(struct spdk_json_write_ctx *w, struct spdk_accel_crypto_key *key)
{
	spdk_json_write_object_begin(w);
	__accel_crypto_key_dump_param(w, key);
	spdk_json_write_object_end(w);
}

static void
_accel_crypto_key_write_config_json(struct spdk_json_write_ctx *w,
				    struct spdk_accel_crypto_key *key)
{
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "accel_crypto_key_create");
	spdk_json_write_named_object_begin(w, "params");
	__accel_crypto_key_dump_param(w, key);
	spdk_json_write_object_end(w);
	spdk_json_write_object_end(w);
}

static void
_accel_crypto_keys_write_config_json(struct spdk_json_write_ctx *w, bool full_dump)
{
	struct spdk_accel_crypto_key *key;

	spdk_spin_lock(&g_keyring_spin);
	TAILQ_FOREACH(key, &g_keyring, link) {
		if (full_dump) {
			_accel_crypto_key_write_config_json(w, key);
		} else {
			_accel_crypto_key_dump_param(w, key);
		}
	}
	spdk_spin_unlock(&g_keyring_spin);
}

void
_accel_crypto_keys_dump_param(struct spdk_json_write_ctx *w)
{
	_accel_crypto_keys_write_config_json(w, false);
}

void
spdk_accel_write_config_json(struct spdk_json_write_ctx *w)
{
	struct spdk_accel_module_if *accel_module;
	int i;

	/*
	 * The accel fw has no config, there may be some in
	 * the modules though.
	 */
	spdk_json_write_array_begin(w);
	TAILQ_FOREACH(accel_module, &spdk_accel_module_list, tailq) {
		if (accel_module->write_config_json) {
			accel_module->write_config_json(w);
		}
	}
	for (i = 0; i < ACCEL_OPC_LAST; i++) {
		if (g_modules_opc_override[i]) {
			accel_write_overridden_opc(w, g_opcode_strings[i], g_modules_opc_override[i]);
		}
	}

	_accel_crypto_keys_write_config_json(w, true);

	spdk_json_write_array_end(w);
}

void
spdk_accel_module_finish(void)
{
	if (!g_accel_module) {
		g_accel_module = TAILQ_FIRST(&spdk_accel_module_list);
	} else {
		g_accel_module = TAILQ_NEXT(g_accel_module, tailq);
	}

	if (!g_accel_module) {
		spdk_spin_destroy(&g_keyring_spin);
		accel_module_finish_cb();
		return;
	}

	if (g_accel_module->module_fini) {
		spdk_thread_send_msg(spdk_get_thread(), g_accel_module->module_fini, NULL);
	} else {
		spdk_accel_module_finish();
	}
}

void
spdk_accel_finish(spdk_accel_fini_cb cb_fn, void *cb_arg)
{
	struct spdk_accel_crypto_key *key, *key_tmp;
	enum accel_opcode op;

	assert(cb_fn != NULL);

	g_fini_cb_fn = cb_fn;
	g_fini_cb_arg = cb_arg;

	spdk_spin_lock(&g_keyring_spin);
	TAILQ_FOREACH_SAFE(key, &g_keyring, link, key_tmp) {
		accel_crypto_key_destroy_unsafe(key);
	}
	spdk_spin_unlock(&g_keyring_spin);

	for (op = 0; op < ACCEL_OPC_LAST; op++) {
		if (g_modules_opc_override[op] != NULL) {
			free(g_modules_opc_override[op]);
			g_modules_opc_override[op] = NULL;
		}
		g_modules_opc[op].module = NULL;
	}

	spdk_io_device_unregister(&spdk_accel_module_list, NULL);
	spdk_accel_module_finish();
}

SPDK_LOG_REGISTER_COMPONENT(accel)
