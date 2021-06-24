#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

rpc_py="$rootdir/scripts/rpc.py"
MEM_SCRIPT="$rootdir/scripts/dpdk_mem_info.py"

"${SPDK_APP[@]}" &
spdkpid=$!

waitforlisten $spdkpid

trap 'killprocess $spdkpid' SIGINT SIGTERM EXIT

$rpc_py env_dpdk_get_mem_stats

$MEM_SCRIPT

$MEM_SCRIPT -m 0

trap - SIGINT SIGTERM EXIT
killprocess $spdkpid
