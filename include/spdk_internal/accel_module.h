/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES
 *   All rights reserved.
 */

#ifndef SPDK_ACCEL_MODULE_H
#define SPDK_ACCEL_MODULE_H

#include "spdk/stdinc.h"

#include "spdk/accel.h"
#include "spdk/queue.h"
#include "spdk/config.h"

struct spdk_accel_module_if;
struct spdk_accel_task;

void spdk_accel_task_complete(struct spdk_accel_task *task, int status);

/** Some reasonable key length used with strnlen() */
#define SPDK_ACCEL_CRYPTO_KEY_MAX_HEX_LENGTH (256 + 1)

struct spdk_accel_crypto_key {
	void *priv;					/**< Module private data */
	char *key;					/**< Key in binary form */
	size_t key_size;				/**< Key size in bytes */
	char *key2;					/**< Key2 in binary form */
	size_t key2_size;				/**< Key2 size in bytes */
	struct spdk_accel_module_if *module_if;			/**< Accel module the key belongs to */
	struct spdk_accel_crypto_key_create_param param;	/**< User input parameters */
	TAILQ_ENTRY(spdk_accel_crypto_key) link;
};

/**
 * Describes user's buffers in remote memory domains in case a module doesn't support memory domains
 * and accel needs to pull/push the data before submitting a task.  Should only be used by accel
 * itself and should not be touched by accel modules.
 */
struct spdk_accel_bounce_buffer {
	struct iovec			*orig_iovs;
	uint32_t			orig_iovcnt;
	struct spdk_memory_domain	*orig_domain;
	void				*orig_domain_ctx;
	struct iovec			iov;
};

enum spdk_accel_aux_iov_type {
	SPDK_ACCEL_AUX_IOV_SRC,
	SPDK_ACCEL_AUX_IOV_DST,
	SPDK_ACCEL_AUX_IOV_SRC2,
	SPDK_ACCEL_AUX_IOV_DST2,
	SPDK_ACCEL_AUX_IOV_MAX,
};

struct spdk_accel_task {
	struct accel_io_channel		*accel_ch;
	spdk_accel_completion_cb	cb_fn;
	void				*cb_arg;
	spdk_accel_step_cb		step_cb_fn;
	void				*step_cb_arg;
	struct spdk_memory_domain	*src_domain;
	void				*src_domain_ctx;
	struct spdk_memory_domain	*dst_domain;
	void				*dst_domain_ctx;
	struct {
		struct iovec		*iovs; /* iovs passed by the caller */
		uint32_t		iovcnt; /* iovcnt passed by the caller */
	} s;
	union {
		struct {
			struct iovec		*iovs; /* iovs passed by the caller */
			uint32_t		iovcnt; /* iovcnt passed by the caller */
		} d;
		struct {
			struct iovec		*iovs;
			uint32_t		iovcnt;
		} s2;
	};
	union {
		struct {
			struct iovec		*iovs;
			uint32_t		iovcnt;
		} d2;
		uint32_t			seed;
		uint64_t			fill_pattern;
		struct spdk_accel_crypto_key	*crypto_key;
	};
	union {
		uint32_t		*crc_dst;
		uint32_t		*output_size;
		uint32_t		block_size; /* for crypto op */
	};
	struct {
		struct spdk_accel_bounce_buffer s;
		struct spdk_accel_bounce_buffer d;
	} bounce;
	enum accel_opcode		op_code;
	uint64_t			iv; /* Initialization vector (tweak) for crypto op */
	int				flags;
	int				status;
	struct iovec			aux_iovs[SPDK_ACCEL_AUX_IOV_MAX];
	TAILQ_ENTRY(spdk_accel_task)	link;
	TAILQ_ENTRY(spdk_accel_task)	seq_link;
};

struct spdk_accel_module_if {
	/** Initialization function for the module.  Called by the spdk
	 *   application during startup.
	 *
	 *  Modules are required to define this function.
	 */
	int	(*module_init)(void);

	/** Finish function for the module.  Called by the spdk application
	 *   before the spdk application exits to perform any necessary cleanup.
	 *
	 *  Modules are not required to define this function.
	 */
	void	(*module_fini)(void *ctx);

	/**
	 * Write Acceleration module configuration into provided JSON context.
	 */
	void	(*write_config_json)(struct spdk_json_write_ctx *w);

	/**
	 * Returns the allocation size required for the modules to use for context.
	 */
	size_t	(*get_ctx_size)(void);

	const char *name;
	bool (*supports_opcode)(enum accel_opcode);
	struct spdk_io_channel *(*get_io_channel)(void);
	int (*submit_tasks)(struct spdk_io_channel *ch, struct spdk_accel_task *accel_task);

	/**
	 * Create crypto key function. Module is responsible to fill all necessary parameters in
	 * \b spdk_accel_crypto_key structure
	 */
	int (*crypto_key_init)(struct spdk_accel_crypto_key *key);
	void (*crypto_key_deinit)(struct spdk_accel_crypto_key *key);

	/**
	 * Returns memory domains supported by the module.  If NULL, the module does not support
	 * memory domains.  The `domains` array can be NULL, in which case this function only
	 * returns the number of supported memory domains.
	 *
	 * \param domains Memory domain array.
	 * \param num_domains Size of the `domains` array.
	 *
	 * \return Number of supported memory domains.
	 */
	int (*get_memory_domains)(struct spdk_memory_domain **domains, int num_domains);

	TAILQ_ENTRY(spdk_accel_module_if)	tailq;
};

void spdk_accel_module_list_add(struct spdk_accel_module_if *accel_module);

#define SPDK_ACCEL_MODULE_REGISTER(name, module) \
static void __attribute__((constructor)) _spdk_accel_module_register_##name(void) \
{ \
	spdk_accel_module_list_add(module); \
}

/**
 * Called by an accel module when cleanup initiated during .module_fini has completed
 */
void spdk_accel_module_finish(void);

#endif
