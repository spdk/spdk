#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 Nutanix Inc.
#  All rights reserved.
#
# System-level test for NVM command set processing limits (DMRSL, WZSL)
# exposed through NVMe-oF.
#
# The SPDK initiator discovers limits via NVM Identify Controller data
# and the bdev layer splits I/Os that exceed them before sending to the
# target.  Exercises the initiator-side split path with I/O sizes below,
# at, and above each boundary.
#
# Unmap data-level side effects (read-after-deallocate) are NOT verified
# because the NVMe spec does not guarantee what values are returned for
# deallocated blocks (see DLFEAT/DRAT).
#
# Write zeroes side effects ARE verified in test/nvmf/target/bdevio.sh
# which runs bdevio against a WZSL-limited subsystem, performing
# write→write_zeroes→read→compare-to-zero at multiple sizes (including
# sizes exceeding WZSL that require bdev-layer splitting).
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512
NQN=nqn.2016-06.io.spdk:cnode1
# DMRSL = 8 logical blocks (4 KiB with 512B block size).
DMRSL=8
DMRSL_BYTES=$((DMRSL * MALLOC_BLOCK_SIZE))
# WZSL = 1 (2^(1+12)/512 = 16 blocks (8 KiB)).
WZSL=1
WZSL_BLOCKS=$(((1 << (WZSL + 12)) / MALLOC_BLOCK_SIZE))
WZSL_BYTES=$((WZSL_BLOCKS * MALLOC_BLOCK_SIZE))

function cleanup() {
	process_shm --id $NVMF_APP_SHM_ID || true
	rm -f "$testdir/nvm_limits_bdevperf.conf"
	nvmftestfini
}

nvmftestinit
nvmfappstart -m 0x1

$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
$rpc_py nvmf_create_subsystem $NQN -a --dmrsl $DMRSL --wzsl $WZSL
$rpc_py nvmf_subsystem_add_ns $NQN Malloc0
$rpc_py nvmf_subsystem_add_listener $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

trap 'cleanup; exit 1' SIGINT SIGTERM EXIT

# Three unmap jobs probing the DMRSL boundary:
#   below:  DMRSL-1 blocks  (no split required)
#   at:     DMRSL   blocks  (exactly at the limit)
#   above:  DMRSL+1 blocks  (initiator must split)
cat > "$testdir/nvm_limits_bdevperf.conf" << EOF
[global]
filename=Nvme1n1
iodepth=4

[trim_below_limit]
rw=unmap
bs=$((DMRSL_BYTES - MALLOC_BLOCK_SIZE))

[trim_at_limit]
rw=unmap
bs=$DMRSL_BYTES

[trim_above_limit]
rw=unmap
bs=$((DMRSL_BYTES + MALLOC_BLOCK_SIZE))

[write_zeroes_below_limit]
rw=write_zeroes
bs=$((WZSL_BYTES - MALLOC_BLOCK_SIZE))

[write_zeroes_at_limit]
rw=write_zeroes
bs=$WZSL_BYTES

[write_zeroes_above_limit]
rw=write_zeroes
bs=$((WZSL_BYTES + MALLOC_BLOCK_SIZE))
EOF

run_app "$SPDK_EXAMPLE_DIR/bdevperf" -m 0x2 \
	--json <(gen_nvmf_target_json) -j "$testdir/nvm_limits_bdevperf.conf" -t 1

# Verify unmap operations reached the target's backing bdev.
bytes_unmapped=$($rpc_py bdev_get_iostat -b Malloc0 | jq -r '.bdevs[0].bytes_unmapped')
[[ $bytes_unmapped -gt 0 ]]

$rpc_py nvmf_delete_subsystem $NQN

trap - SIGINT SIGTERM EXIT

cleanup
