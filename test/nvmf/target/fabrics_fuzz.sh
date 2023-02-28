#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

nvmftestinit

"${NVMF_APP[@]}" -m 0x1 > $output_dir/nvmf_fuzz_tgt_output.txt 2>&1 &
nvmfpid=$!

trap 'process_shm --id $NVMF_APP_SHM_ID; killprocess $nvmfpid; nvmftestfini $1; exit 1' SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

$rpc_py bdev_malloc_create -b Malloc0 64 512

$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

trid="trtype:$TEST_TRANSPORT adrfam:IPv4 subnqn:nqn.2016-06.io.spdk:cnode1 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT"

# Note that we chose a consistent seed to ensure that this test is consistent in nightly builds.
$rootdir/test/app/fuzz/nvme_fuzz/nvme_fuzz -m 0x2 -r "/var/tmp/nvme_fuzz" -t 30 -S 123456 -F "$trid" -N -a 2> $output_dir/nvmf_fuzz_logs1.txt
# We don't specify a seed for this test. Instead we run a static list of commands from example.json.
$rootdir/test/app/fuzz/nvme_fuzz/nvme_fuzz -m 0x2 -r "/var/tmp/nvme_fuzz" -F "$trid" -j $rootdir/test/app/fuzz/nvme_fuzz/example.json -a 2> $output_dir/nvmf_fuzz_logs2.txt

$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmftestfini
rm "$output_dir/"nvmf_fuzz_logs*.txt
