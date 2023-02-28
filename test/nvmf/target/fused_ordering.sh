#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

nvmftestinit
nvmfappstart -m 0x2

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -m 10
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py bdev_null_create NULL1 1000 512
$rpc_py bdev_wait_for_examine
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 NULL1

$rootdir/test/nvme/fused_ordering/fused_ordering -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT subnqn:nqn.2016-06.io.spdk:cnode1"

trap - SIGINT SIGTERM EXIT

nvmftestfini
