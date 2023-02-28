#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  Copyright (C) 2022 Nutanix Inc.
#  All rights reserved.
#

# Test --rpcs-allowed: only the given RPCs are allowed and reported.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

trap 'killprocess $spdk_tgt_pid; exit 1' ERR

$SPDK_BIN_DIR/spdk_tgt --rpcs-allowed spdk_get_version,rpc_get_methods &
spdk_tgt_pid=$!
waitforlisten $spdk_tgt_pid

$rootdir/scripts/rpc.py spdk_get_version
declare -a methods=($(rpc_cmd rpc_get_methods | jq -rc ".[]"))
[[ "${methods[0]}" = "spdk_get_version" ]]
[[ "${methods[1]}" = "rpc_get_methods" ]]
[[ "${#methods[@]}" = 2 ]]

NOT $rootdir/scripts/rpc.py env_dpdk_get_mem_stats

killprocess $spdk_tgt_pid
