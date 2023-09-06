/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2020 Intel Corporation.
 *   Copyright (c) 2022, 2023 NVIDIA CORPORATION & AFFILIATES
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

enum spdk_accel_crypto_tweak_mode {
	/* Tweak[127:0] = {64'b0, LBA[63:0]} */
	SPDK_ACCEL_CRYPTO_TWEAK_MODE_SIMPLE_LBA,

	/* Tweak[127:0] = {1’b0, ~LBA[62:0], LBA[63:0]} */
	SPDK_ACCEL_CRYPTO_TWEAK_MODE_JOIN_NEG_LBA_WITH_LBA,

	/* Tweak is derived from LBA that is internally incremented by 1 for every 512 bytes processed
	 * so initial lba = (BLOCK_SIZE_IN_BYTES / 512) * LBA
	 * Tweak[127:0] = {lba[127:0]} */
	SPDK_ACCEL_CRYPTO_TWEAK_MODE_INCR_512_FULL_LBA,

	/* Tweak is derived from LBA that is internally incremented by 1 for every 512 bytes processed
	 * so initial lba = (BLOCK_SIZE_IN_BYTES / 512) * LBA
	 * Tweak[127:0] = {lba[63:0], 64'b0} */
	SPDK_ACCEL_CRYPTO_TWEAK_MODE_INCR_512_UPPER_LBA,
};

struct spdk_accel_crypto_key {
	void *priv;					/**< Module private data */
	char *key;					/**< Key in binary form */
	size_t key_size;				/**< Key size in bytes */
	char *key2;					/**< Key2 in binary form */
	size_t key2_size;				/**< Key2 size in bytes */
	enum spdk_accel_cipher cipher;
	enum spdk_accel_crypto_tweak_mode tweak_mode;
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
	SPDK_ACCEL_AXU_IOV_VIRT_SRC,
	SPDK_ACCEL_AXU_IOV_VIRT_DST,
	SPDK_ACCEL_AUX_IOV_MAX,
};

struct spdk_accel_task_aux_data {
	SLIST_ENTRY(spdk_accel_task_aux_data) link;
	struct iovec iovs[SPDK_ACCEL_AUX_IOV_MAX];
	struct {
		struct spdk_accel_bounce_buffer s;
		struct spdk_accel_bounce_buffer d;
	} bounce;
};

struct spdk_accel_task {
	TAILQ_ENTRY(spdk_accel_task)	seq_link;
	STAILQ_ENTRY(spdk_accel_task)	link;
	/* Uses enum spdk_accel_opcode */
	uint8_t				op_code;
	uint8_t				flags;
	bool				has_aux;
	int16_t				status;
	struct accel_io_channel		*accel_ch;
	struct spdk_accel_sequence	*seq;
	union {
		/* Used by spdk_accel_submit_* functions */
		spdk_accel_completion_cb	cb_fn;
		/* Used by spdk_accel_append_* functions */
		spdk_accel_step_cb		step_cb_fn;
	};
	void				*cb_arg;
	struct spdk_memory_domain	*src_domain;
	void				*src_domain_ctx;
	struct spdk_memory_domain	*dst_domain;
	void				*dst_domain_ctx;
	uint64_t			nbytes;
	union {
		struct {
			struct iovec		*iovs; /* iovs passed by the caller */
			uint32_t		iovcnt; /* iovcnt passed by the caller */
		} s;
		struct {
			void			**srcs;
			uint32_t		cnt;
		} nsrcs;
	};
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
		struct {
			const struct spdk_dif_ctx	*ctx;
			struct spdk_dif_error		*err;
			uint32_t	num_blocks;
		} dif;
	};
	union {
		uint32_t		*crc_dst;
		uint32_t		*output_size;
		uint32_t		block_size; /* for crypto op */
	};
	uint64_t			iv; /* Initialization vector (tweak) for crypto op */
	struct spdk_accel_task_aux_data	*aux;
};

struct spdk_accel_opcode_info {
	/**
	 * Minimum buffer alignment required to execute the operation, expressed as power of 2.  The
	 * value of 0 means that the buffers don't need to be aligned.
	 */
	uint8_t required_alignment;
};

struct spdk_accel_module_if {
	/** Name of the module. */
	const char *name;

	/**
	 * Priority of the module.  It's used to select a module to execute an operation when
	 * multiple modules support it.  Higher value means higher priority.  Software module has a
	 * priority of `SPDK_ACCEL_SW_PRIORITY`.  Of course, this value is only relevant when none
	 * of the modules have been explicitly assigned to execute a given operation via
	 * `spdk_accel_assign_opc()`.
	 */
	int priority;

	/**
	 * Initialization function for the module.  Called by the application during startup.
	 *
	 * Modules are required to define this function.
	 */
	int	(*module_init)(void);

	/**
	 * Finish function for the module.  Called by the application before the application exits
	 * to perform any necessary cleanup.
	 *
	 * Modules are not required to define this function.
	 */
	void	(*module_fini)(void *ctx);

	/** Write Acceleration module configuration into provided JSON context. */
	void	(*write_config_json)(struct spdk_json_write_ctx *w);

	/** Returns the allocation size required for the modules to use for context. */
	size_t	(*get_ctx_size)(void);

	/** Reports whether the module supports a given operation. */
	bool (*supports_opcode)(enum spdk_accel_opcode);

	/** Returns module's IO channel on the calling thread. */
	struct spdk_io_channel *(*get_io_channel)(void);

	/**
	 * Submit tasks to be executed by the module.  Once a task execution is done, the module is
	 * required to complete it using `spdk_accel_task_complete()`.  `ch` is the IO channel
	 * obtained by `get_io_channel()`.
	 */
	int (*submit_tasks)(struct spdk_io_channel *ch, struct spdk_accel_task *accel_task);

	/**
	 * Create crypto key function. Module is responsible to fill all necessary parameters in
	 * \b spdk_accel_crypto_key structure
	 */
	int (*crypto_key_init)(struct spdk_accel_crypto_key *key);

	/** Free any resources associated with `key` allocated during `crypto_key_init()`. */
	void (*crypto_key_deinit)(struct spdk_accel_crypto_key *key);

	/**
	 * Returns true if given tweak mode is supported. If module doesn't implement that function it shall support SIMPLE LBA mode.
	 */
	bool (*crypto_supports_tweak_mode)(enum spdk_accel_crypto_tweak_mode tweak_mode);

	/**
	 * Returns true if given pair (cipher, key size) is supported.
	 */
	bool (*crypto_supports_cipher)(enum spdk_accel_cipher cipher, size_t key_size);

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

	/**
	 * Returns information/constraints for a given operation.  If unimplemented, it is assumed
	 * that the module doens't have any constraints to execute any operation.
	 */
	int (*get_operation_info)(enum spdk_accel_opcode opcode,
				  const struct spdk_accel_operation_exec_ctx *ctx,
				  struct spdk_accel_opcode_info *info);

	TAILQ_ENTRY(spdk_accel_module_if)	tailq;
};

void spdk_accel_module_list_add(struct spdk_accel_module_if *accel_module);

#define SPDK_ACCEL_MODULE_REGISTER(name, module) \
static void __attribute__((constructor)) _spdk_accel_module_register_##name(void) \
{ \
	spdk_accel_module_list_add(module); \
}

/* Priority of the accel_sw module */
#define SPDK_ACCEL_SW_PRIORITY (-1)

/**
 * Called by an accel module when cleanup initiated during .module_fini has completed
 */
void spdk_accel_module_finish(void);

/**
 * Platform driver responsible for executing tasks in a sequence.  If no driver is selected, tasks
 * are submitted to accel modules.  All drivers are required to be aware of memory domains.
 */
struct spdk_accel_driver {
	/** Name of the driver. */
	const char *name;

	/** Initializes the driver, called when accel initializes.  Optional. */
	int (*init)(void);

	/** Performs cleanup on resources allocated by the driver.  Optional. */
	void (*fini)(void);

	/**
	 * Executes a sequence of accel operations.  The driver should notify accel about each
	 * completed task using `spdk_accel_task_complete()`.  Once all tasks are completed or the
	 * driver cannot proceed with a given task (e.g. because it doesn't handle specific opcode),
	 * accel should be notified via `spdk_accel_sequence_continue()`.  If there are tasks left
	 * in a sequence, the first will be submitted to a module, while the rest will be sent back
	 * to the driver.  `spdk_accel_sequence_continue()` should only be called if this function
	 * succeeds (i.e. returns 0).
	 *
	 * \param ch IO channel obtained by `get_io_channel()`.
	 * \param seq Sequence of tasks to execute.
	 *
	 * \return 0 on success, negative errno on failure.
	 */
	int (*execute_sequence)(struct spdk_io_channel *ch, struct spdk_accel_sequence *seq);

	/** Returns IO channel that will be passed to `execute_sequence()`. */
	struct spdk_io_channel *(*get_io_channel)(void);

	/**
	 * Returns information/constraints for a given operation.  If unimplemented, it is assumed
	 * that the driver doesn't have any constraints to execute any operation.
	 */
	int (*get_operation_info)(enum spdk_accel_opcode opcode,
				  const struct spdk_accel_operation_exec_ctx *ctx,
				  struct spdk_accel_opcode_info *info);

	TAILQ_ENTRY(spdk_accel_driver)	tailq;
};

/**
 * Notifies accel that a driver has finished executing a sequence (or its part) and accel should
 * continue processing it.
 *
 * \param seq Sequence object.
 */
void spdk_accel_sequence_continue(struct spdk_accel_sequence *seq);

void spdk_accel_driver_register(struct spdk_accel_driver *driver);

#define SPDK_ACCEL_DRIVER_REGISTER(name, driver) \
static void __attribute__((constructor)) _spdk_accel_driver_register_##name(void) \
{ \
	spdk_accel_driver_register(driver); \
}

typedef void (*spdk_accel_sequence_get_buf_cb)(struct spdk_accel_sequence *seq, void *cb_arg);

/**
 * Allocates memory for an accel buffer in a given sequence.  The callback is only executed if the
 * buffer couldn't be allocated immediately.
 *
 * \param seq Sequence object.
 * \param buf Accel buffer to allocate.
 * \param domain Accel memory domain.
 * \param domain_ctx Memory domain context.
 * \param cb_fn Callback to be executed once the buffer is allocated.
 * \param cb_ctx Argument to be passed to `cb_fn`.
 *
 * \return true if the buffer was immediately allocated, false otherwise.
 */
bool spdk_accel_alloc_sequence_buf(struct spdk_accel_sequence *seq, void *buf,
				   struct spdk_memory_domain *domain, void *domain_ctx,
				   spdk_accel_sequence_get_buf_cb cb_fn, void *cb_ctx);

/**
 * Returns the first task remaining to be executed in a given sequence.
 *
 * \param seq Sequence object.
 *
 * \return the first remaining task or NULL if all tasks are already completed.
 */
struct spdk_accel_task *spdk_accel_sequence_first_task(struct spdk_accel_sequence *seq);

/**
 * Returns the next remaining task that follows a given task in a sequence.
 *
 * \param task Accel task.  This task must be still oustanding (i.e. it wasn't completed through
 *             `spdk_accel_task_complete()`).
 *
 * \return the next task or NULL if `task` was the last task in a sequence.
 */
struct spdk_accel_task *spdk_accel_sequence_next_task(struct spdk_accel_task *task);

/**
 * Returns an accel module identified by `name`.
 *
 * \param name Name of the module.
 *
 * \return Pointer to a module or NULL if it couldn't be found.
 */
struct spdk_accel_module_if *spdk_accel_get_module(const char *name);

#endif
