#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

TEST_TIMEOUT=1200

# This argument is used in addition to the test arguments in autotest_common.sh
for i in "$@"; do
	case "$i" in
		--timeout=*)
			TEST_TIMEOUT="${i#*=}"
			;;
	esac
done

nvmftestinit

timing_enter nvmf_fuzz_test

trid="trtype:$TEST_TRANSPORT adrfam:IPv4 subnqn:nqn.2016-06.io.spdk:cnode1 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT"

"${NVMF_APP[@]}" -m 0xF &> "$output_dir/nvmf_autofuzz_tgt_output.txt" &
nvmfpid=$!

trap 'process_shm --id $NVMF_APP_SHM_ID; killprocess $nvmfpid; nvmftestfini $1; exit 1' SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192

$rpc_py bdev_malloc_create -b Malloc0 64 512

$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# Note that we chose a consistent seed to ensure that this test is consistent in nightly builds.
$rootdir/test/app/fuzz/nvme_fuzz/nvme_fuzz -m 0xF0 -r "/var/tmp/nvme_fuzz" -t $TEST_TIMEOUT -F "$trid" -N -a 2> $output_dir/nvmf_autofuzz_logs.txt

$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmfcleanup
nvmftestfini
timing_exit nvmf_fuzz_test
