/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Advanced Micro Devices, Inc.
 *   All rights reserved.
 */

/** \file
 * AMD NTB specification / on-the-wire protocol definitions.
 *
 * This header is intentionally split out of include/spdk/ntb_amd.h so
 * that consumers of the public driver API don't have to pull in the
 * register layout, while applications that participate in the
 * peer-to-peer scratchpad handshake (e.g. examples/ntb/perf) can.
 */

#ifndef SPDK_NTB_AMD_SPEC_H
#define SPDK_NTB_AMD_SPEC_H

#include "spdk/stdinc.h"
#include "spdk/assert.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bit-position helper. Defined here (with a guard) because every consumer
 * of this public header expands the AMD_* register-event macros below, so
 * the helper has to be available without depending on a private header.
 */
#ifndef BIT
#define BIT(n) (1UL << (n))
#endif

#define NTB_SPAD_USER               "spad_user_"
#define NTB_SPAD_USER_LEN           (sizeof(NTB_SPAD_USER) - 1)
#define NTB_SPAD_USER_MAX_NUM       4
#define NTB_ATTR_NAME_LEN           30

#define NTB_PCIE_BAR 0

#define NTB_MAX_HW_QUEUES		16
/* AMD NTB registers */
#define AMD_DB_COUNT            16
#define AMD_MSIX_VECTOR_COUNT   24
#define AMD_SPADS_COUNT         16
#define AMD_MW_COUNT            2
#define AMD_MW_IDX              2

#define AMD_PEER_OFFSET         0x400
#define AMD_SIDEINFO_OFFSET     0x408
#define AMD_DBFM_OFFSET         0x450
#define AMD_DBREQ_OFFSET        0x454
#define AMD_DBMASK_OFFSET       0x45C
#define AMD_DBSTAT_OFFSET       0x460
#define AMD_INTMASK_OFFSET      0x470
#define AMD_INTSTAT_OFFSET      0x474
#define AMD_PMESTAT_OFFSET      0x480

/* event type */
#define AMD_PEER_FLUSH_EVENT    BIT(0)
#define AMD_PEER_RESET_EVENT    BIT(1)
#define AMD_PEER_D3_EVENT       BIT(2)
#define AMD_PEER_PMETO_EVENT    BIT(3)
#define AMD_PEER_D0_EVENT       BIT(4)
#define AMD_LINK_UP_EVENT       BIT(5)
#define AMD_LINK_DOWN_EVENT     BIT(6)
#define AMD_EVENT_INTMASK       (AMD_PEER_FLUSH_EVENT | AMD_PEER_RESET_EVENT | \
                AMD_PEER_D3_EVENT | AMD_PEER_PMETO_EVENT | AMD_PEER_D0_EVENT | \
                AMD_LINK_UP_EVENT | AMD_LINK_DOWN_EVENT)

/* Bar Registers */
#define AMD_BAR23_XLAT_OFFSET   0x438
#define AMD_BAR23_LIMIT_OFFSET  0x418
#define AMD_BAR45_XLAT_OFFSET   0x440
#define AMD_BAR45_LIMIT_OFFSET  0x420

#define AMD_SIDE_MASK  BIT(0)
#define AMD_SIDE_READY BIT(1)

#define AMD_SPAD_OFFSET         0x210
#define AMD_CNTL_OFFSET         0x200

#define PMM_REG_CTL             BIT(21)
#define SMM_REG_CTL             BIT(20)

/* SMU registers */
#define AMD_SMUACK_OFFSET               0x4A0

#define AMD_NTB_LNK_STA_ACTIVE_BIT          0x2000
#define AMD_NTB_LNK_STA_SPEED_MASK          0x000f
#define AMD_NTB_LNK_STA_WIDTH_MASK          0x03f0
#define AMD_NTB_LNK_STA_ACTIVE(x)           (!!((x) & AMD_NTB_LNK_STA_ACTIVE_BIT))
#define AMD_NTB_LNK_STA_SPEED(x)            ((x) & AMD_NTB_LNK_STA_SPEED_MASK)
#define AMD_NTB_LNK_STA_WIDTH(x)            (((x) & AMD_NTB_LNK_STA_WIDTH_MASK) >> 4)

enum spdk_ntb_bar {
	NTB_BAR23 = 2,
	NTB_BAR45 = 4,
};

/**
 * NTB topology assignment, as exchanged over the link and returned by
 * spdk_ntb_amd_attr_get(SPDK_NTB_TOPO_NAME).
 *
 * One side comes up as primary (PRI), the other as secondary (SEC); the
 * AMD NTB exposes this through bit 0 of the side-info register and the
 * driver caches it here.
 */
enum spdk_ntb_topo {
	NTB_TOPO_NONE = 0,
	NTB_TOPO_PRI,
	NTB_TOPO_SEC,
};

/**
 * NTB link state, as returned by spdk_ntb_amd_attr_get(NTB_LINK_STATUS_NAME).
 */
enum spdk_ntb_link {
	NTB_LINK_DOWN = 0,
	NTB_LINK_UP,
};

/**
 * Layout of the peer-to-peer handshake scratchpad block.
 *
 * Both sides agree on these indices when publishing / consuming the
 * memory-window configuration via the scratchpad registers. Index 0 is
 * intentionally reserved (the AMD NTB hardware itself uses spad[0] for
 * peer-discovery interlock).
 *
 * Only the memory-window count is exchanged: the actual address
 * translation is programmed directly into the peer's XLAT registers by
 * ntb_amd_mw_set_trans(), so there is no need to publish per-window base
 * addresses or sizes through the scratchpads.
 */
enum spdk_ntb_spad_idx {
	SPAD_NUM_MWS = 1,
};

/*
 * MMIO access notes for AMD NTB:
 *
 * - All AMD NTB control/status registers are little-endian. The driver
 *   currently only targets little-endian hosts (x86_64), so SPDK's typed
 *   spdk_mmio_read_N / spdk_mmio_write_N helpers (which boil down to a
 *   barrier + volatile aligned access in host byte order) are correct.
 *   If this driver is ever ported to a big-endian host, every register
 *   access must be wrapped in an explicit le32toh() / htole32() (and
 *   equivalents for 16-bit) at the call site.
 *
 * - The helpers also require natural alignment of each MMIO offset.
 *   Verify here so any future register addition that violates this
 *   immediately fails the build instead of silently going through a
 *   split/atomic-busted access at runtime.
 */
SPDK_STATIC_ASSERT((AMD_DBSTAT_OFFSET     & 0x1) == 0, "AMD_DBSTAT_OFFSET must be 2-byte aligned");
SPDK_STATIC_ASSERT((AMD_DBMASK_OFFSET     & 0x1) == 0, "AMD_DBMASK_OFFSET must be 2-byte aligned");
SPDK_STATIC_ASSERT((AMD_DBREQ_OFFSET      & 0x1) == 0, "AMD_DBREQ_OFFSET must be 2-byte aligned");
SPDK_STATIC_ASSERT((AMD_DBFM_OFFSET       & 0x1) == 0, "AMD_DBFM_OFFSET must be 2-byte aligned");
SPDK_STATIC_ASSERT((AMD_SIDEINFO_OFFSET   & 0x3) == 0,
		   "AMD_SIDEINFO_OFFSET must be 4-byte aligned");
SPDK_STATIC_ASSERT((AMD_CNTL_OFFSET       & 0x3) == 0, "AMD_CNTL_OFFSET must be 4-byte aligned");
SPDK_STATIC_ASSERT((AMD_INTMASK_OFFSET    & 0x3) == 0, "AMD_INTMASK_OFFSET must be 4-byte aligned");
SPDK_STATIC_ASSERT((AMD_INTSTAT_OFFSET    & 0x3) == 0, "AMD_INTSTAT_OFFSET must be 4-byte aligned");
SPDK_STATIC_ASSERT((AMD_PMESTAT_OFFSET    & 0x3) == 0, "AMD_PMESTAT_OFFSET must be 4-byte aligned");
SPDK_STATIC_ASSERT((AMD_BAR23_XLAT_OFFSET & 0x3) == 0,
		   "AMD_BAR23_XLAT_OFFSET must be 4-byte aligned");
SPDK_STATIC_ASSERT((AMD_BAR23_LIMIT_OFFSET & 0x3) == 0,
		   "AMD_BAR23_LIMIT_OFFSET must be 4-byte aligned");
SPDK_STATIC_ASSERT((AMD_BAR45_XLAT_OFFSET & 0x3) == 0,
		   "AMD_BAR45_XLAT_OFFSET must be 4-byte aligned");
SPDK_STATIC_ASSERT((AMD_BAR45_LIMIT_OFFSET & 0x3) == 0,
		   "AMD_BAR45_LIMIT_OFFSET must be 4-byte aligned");
SPDK_STATIC_ASSERT((AMD_SPAD_OFFSET       & 0x3) == 0, "AMD_SPAD_OFFSET must be 4-byte aligned");


#ifdef __cplusplus
}
#endif

#endif /* SPDK_NTB_AMD_SPEC_H */
