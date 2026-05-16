#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512
NQN=nqn.2016-06.io.spdk:cnode1

function cleanup() {
	process_shm --id $NVMF_APP_SHM_ID || true
	rm -f "$testdir/bdevperf.conf"
	nvmftestfini
}

nvmftestinit
nvmfappstart -m 0x1 --wait-for-rpc

# Minimal number of bdev io pool (5) and cache (1)
$rpc_py bdev_set_options -p 5 -c 1
$rpc_py framework_start_init
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py nvmf_create_subsystem $NQN -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns $NQN Malloc0
$rpc_py nvmf_subsystem_add_listener $NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

trap 'cleanup; exit 1' SIGINT SIGTERM EXIT

cat > "$testdir/bdevperf.conf" << EOF
[global]
filename=Nvme1n1
iodepth=128
bs=4096
[trim]
rw=unmap
[flush]
rw=flush
[write]
rw=write
[read]
rw=read
EOF

run_app "$SPDK_EXAMPLE_DIR/bdevperf" -m 0xE -i 1 --json <(gen_nvmf_target_json) -j "$testdir/bdevperf.conf" -t 1

$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

cleanup
