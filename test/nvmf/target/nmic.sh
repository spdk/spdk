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

nvmftestinit
nvmfappstart -m 0xF

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

# Create subsystems
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s $NVMF_SERIAL
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s "$NVMF_PORT"

echo "test case1: single bdev can't be used in multiple subsystems"
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode2 -a -s SPDK2
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode2 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s "$NVMF_PORT"
nmic_status=0
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode2 Malloc0 || nmic_status=$?

if [ $nmic_status -eq 0 ]; then
	echo " Adding namespace passed - failure expected."
	nvmftestfini
	exit 1
else
	echo " Adding namespace failed - expected result."
fi

echo "test case2: host connect to nvmf target in multiple paths"
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s "$NVMF_SECOND_PORT"
$NVME_CONNECT -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
$NVME_CONNECT -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_SECOND_PORT"

waitforserial "$NVMF_SERIAL"

$rootdir/scripts/fio-wrapper -p nvmf -i 4096 -d 1 -t write -r 1 -v

nvme disconnect -n "nqn.2016-06.io.spdk:cnode1"
waitforserial_disconnect "$NVMF_SERIAL"

trap - SIGINT SIGTERM EXIT

nvmftestfini
