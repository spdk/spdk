#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512
NVME_CONNECT="nvme connect -i 16"

nvmftestinit

nvmfappstart -m 0xF

# create the rdma transport with an intentionally small SRQ depth
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192 -s 1024

for i in $(seq 0 5); do
	$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode$i -a -s "SPDK0000000000000${i}"
	$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc$i
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode$i Malloc$i
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode$i -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
	$NVME_CONNECT -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode${i}" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
	waitforblk "nvme${i}n1"
done

# by running 6 different FIO jobs, each with 13 subjobs, we end up with 78 fio threads trying to write to
# our target at once. This completely overwhelms the target SRQ, but allows us to verify that rnr_retry is
# working even at very high queue depths because the rdma qpair doesn't fail.
# It is normal to see the initiator timeout and reconnect waiting for completions from an overwhelmed target,
# but the connection should come up and FIO should complete without errors.
$rootdir/scripts/fio-wrapper -p nvmf -i 1048576 -d 128 -t read -r 10 -n 13

sync

for i in $(seq 0 5); do
	nvme disconnect -n "nqn.2016-06.io.spdk:cnode${i}"
	waitforserial_disconnect "SPDK0000000000000${i}"
	$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode$i
done

trap - SIGINT SIGTERM EXIT

nvmftestfini
