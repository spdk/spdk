/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 SPDK Hot Upgrade Contributors.
 *   All rights reserved.
 */

/**
 * \file
 * SPDK Hot Upgrade shared state data structures
 */

#ifndef SPDK_HOT_UPGRADE_SHARED_H
#define SPDK_HOT_UPGRADE_SHARED_H

#include "spdk/stdinc.h"
#include "spdk/cpuset.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPDK_HU_IPC_SOCK_PATH     "/var/tmp/spdk_hu_ipc.sock"
#define SPDK_HU_STATE_FILE        "/var/tmp/spdk_hot_upgrade_state"
#define SPDK_HU_CONFIG_FILE       "/var/tmp/spdk_hot_upgrade_config.json"

#define SPDK_HU_MAX_MEMZONES      32
#define SPDK_HU_MAX_MEMPOOLS      32
#define SPDK_HU_MAX_RING_NAMES    32
#define SPDK_HU_MAX_VHOST_DEVS    64
#define SPDK_HU_MAX_MEM_REGIONS   16
#define SPDK_HU_MAX_VHOST_CONNS   16
#define SPDK_HU_MAX_BDEVS         64
#define SPDK_HU_NAME_LEN          64
#define SPDK_HU_PATH_LEN          108

/*
 * Magic number for shared state validation: "SPDK" in ASCII
 */
#define SPDK_HU_STATE_MAGIC        0x5350444B

/*
 * Current shared state version
 */
#define SPDK_HU_STATE_VERSION      1

/**
 * Guest memory region mapping metadata
 */
struct spdk_hu_mem_region {
	uint64_t mmap_addr;      /* Virtual address in Primary */
	uint64_t mmap_size;      /* Mapping size in bytes */
	uint64_t guest_phys_addr;/* Guest physical address */
};

/**
 * DPDK rte_vhost internal connection state
 */
struct spdk_hu_vhost_conn_state {
    int vid;
    uint64_t protocol_features;
    uint64_t negotiated_features;
    uint64_t virtio_features;
};

/**
 * Bdev info for cross-process pointer fixup during hot upgrade.
 * Primary saves (bdev_addr, module_name) pairs; Secondary uses them
 * to fix up bdev->module and bdev->fn_table pointers which point to
 * process-private static globals.
 */
struct spdk_hu_bdev_info {
    uint64_t bdev_addr;                   /* Address of struct spdk_bdev in DMA memory */
    char module_name[SPDK_HU_NAME_LEN];   /* Module name for pointer fixup */
};

/**
 * Shared state structure passed from Primary to Secondary
 */
struct spdk_hot_upgrade_shared_state {
	uint32_t magic;                    /* SPDK_HU_STATE_MAGIC */
	uint32_t version;                  /* SPDK_HU_STATE_VERSION */
	uint64_t base_virtaddr;            /* DPDK hugepage base virtual address */
	int shm_id;                        /* DPDK multi-process shm ID */
	struct spdk_cpuset core_mask;      /* CPU core mask */
	char ipc_sock_path[SPDK_HU_PATH_LEN];

	/* DPDK environment information for Secondary lookup */
	char memzone_names[SPDK_HU_MAX_MEMZONES][SPDK_HU_NAME_LEN];
	char mempool_names[SPDK_HU_MAX_MEMPOOLS][SPDK_HU_NAME_LEN];
	uint32_t num_memzones;
	uint32_t num_mempools;

	/* Ring name to PID mapping */
	uint32_t primary_pid;
	char ring_name_template[SPDK_HU_MAX_RING_NAMES][SPDK_HU_NAME_LEN];

	/* ===== Global management chain head pointers (core of v2) ===== */

	/* bdev subsystem global pointer */
	uint64_t bdev_mgr_addr;            /* Address of g_bdev_mgr pointer */
	uint64_t bdev_io_pool_addr;        /* Address of bdev_io_pool mempool pointer */

	/* bdevs TAILQ head values for cross-process bdev list sharing */
	uint64_t bdevs_first;              /* tqh_first of g_bdev_mgr.bdevs */
	uint64_t bdevs_last;               /* tqh_last  of g_bdev_mgr.bdevs */

	/* Per-bdev info for module/fn_table pointer fixup on Secondary */
	uint32_t num_bdev_infos;
	struct spdk_hu_bdev_info bdev_infos[SPDK_HU_MAX_BDEVS];

	/* vhost subsystem global pointer */
	uint64_t vhost_devices_root;       /* RB_ROOT of g_vhost_devices */
	uint32_t num_vhost_devs;
	uint64_t vhost_dev_addrs[SPDK_HU_MAX_VHOST_DEVS];

	/* vhost user dev additional pointers */
	uint64_t vhost_user_dev_addrs[SPDK_HU_MAX_VHOST_DEVS];
	uint32_t num_vhost_user_devs;

	/* Guest memory mapping metadata */
	uint32_t num_mem_regions;
	struct spdk_hu_mem_region mem_regions[SPDK_HU_MAX_MEM_REGIONS];

	/* DPDK rte_vhost internal connection state */
	uint32_t num_vhost_conns;
	struct spdk_hu_vhost_conn_state vhost_conns[SPDK_HU_MAX_VHOST_CONNS];

	/* RPC addresses */
	char primary_rpc_addr[SPDK_HU_PATH_LEN];
	char secondary_rpc_addr[SPDK_HU_PATH_LEN];
};

/**
 * Save the shared state to a memory-mapped file.
 *
 * \param state Pointer to the shared state to save.
 * \return 0 on success, negative errno on failure.
 */
int spdk_hot_upgrade_state_save(struct spdk_hot_upgrade_shared_state *state);

/**
 * Load the shared state from the memory-mapped file.
 *
 * \param state Output parameter for the loaded state.
 * \return 0 on success, negative errno on failure.
 */
int spdk_hot_upgrade_state_load(struct spdk_hot_upgrade_shared_state **state);

/**
 * Clean up the state file and unmap memory.
 */
void spdk_hot_upgrade_state_file_cleanup(void);

/**
 * Create the IPC Unix Domain Socket (called by Primary).
 *
 * \return Socket fd on success, -1 on failure.
 */
int spdk_hot_upgrade_create_ipc_sock(void);

/**
 * Connect to the IPC Unix Domain Socket (called by Secondary).
 *
 * \return Socket fd on success, -1 on failure.
 */
int spdk_hot_upgrade_connect_ipc_sock(void);

/**
 * Get the current IPC socket fd.
 *
 * \return Socket fd or -1 if not connected.
 */
int spdk_hot_upgrade_get_ipc_sock(void);

/**
 * Send a file descriptor over the IPC socket via SCM_RIGHTS.
 *
 * \param sock IPC socket fd.
 * \param fd File descriptor to send.
 * \return 0 on success, -1 on failure.
 */
int spdk_hot_upgrade_send_fd(int sock, int fd);

/**
 * Receive a file descriptor over the IPC socket via SCM_RIGHTS.
 *
 * \param sock IPC socket fd.
 * \return Received fd on success, -1 on failure.
 */
int spdk_hot_upgrade_recv_fd(int sock);

/**
 * Check if the current process is running as primary.
 *
 * \return true if primary process.
 */
bool spdk_hot_upgrade_is_primary(void);

/**
 * Mark this process as primary or secondary.
 *
 * \param is_primary true for primary, false for secondary.
 */
void spdk_hot_upgrade_set_process_role(bool is_primary);

/**
 * Phase 12: TSC timeline file for IO interruption measurement.
 *
 * Separate from spdk_hot_upgrade_shared_state to avoid struct size
 * incompatibility between old Primary (version 1) and new Secondary.
 */
#define SPDK_HU_TIMELINE_FILE "/var/tmp/spdk_hot_upgrade_timeline"

struct spdk_hu_timeline {
	uint64_t tsc_rate;                    /* spdk_get_ticks_hz() */
	uint64_t tsc_primary_exit_start;      /* Primary: rpc_primary_exit entry */
	uint64_t tsc_primary_drain_done;      /* Primary: drain complete */
	uint64_t tsc_primary_suspend_done;    /* Primary: suspend complete */
	uint64_t tsc_secondary_init_start;    /* Secondary: rpc_secondary_init entry */
	uint64_t tsc_secondary_takeover_done; /* Secondary: subsystem takeover done */
	uint64_t tsc_reactor_running;         /* Secondary: reactor RUNNING, IO resumed */
};

enum spdk_hu_timeline_field {
	SPDK_HU_TSC_PRIMARY_EXIT_START = 0,
	SPDK_HU_TSC_PRIMARY_DRAIN_DONE,
	SPDK_HU_TSC_PRIMARY_SUSPEND_DONE,
	SPDK_HU_TSC_SECONDARY_INIT_START,
	SPDK_HU_TSC_SECONDARY_TAKEOVER_DONE,
	SPDK_HU_TSC_REACTOR_RUNNING,
};

/**
 * Create the timeline file (called by Primary at primary_exit start).
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_hot_upgrade_timeline_create(void);

/**
 * Load the timeline file (called by Secondary at secondary_init start).
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_hot_upgrade_timeline_load(void);

/**
 * Get the loaded timeline pointer.
 *
 * \return Pointer to timeline struct, or NULL if not loaded.
 */
struct spdk_hu_timeline *spdk_hot_upgrade_get_timeline(void);

/**
 * Record a TSC timestamp to the timeline file and msync.
 *
 * \param field Which timeline field to write.
 * \param tsc TSC value (from spdk_get_ticks()).
 */
void spdk_hot_upgrade_timeline_record(enum spdk_hu_timeline_field field, uint64_t tsc);

/**
 * Clean up the timeline file and unmap memory.
 */
void spdk_hot_upgrade_timeline_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_HOT_UPGRADE_SHARED_H */