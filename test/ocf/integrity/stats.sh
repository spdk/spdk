#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#
curdir=$(dirname $(readlink -f "${BASH_SOURCE[0]}"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

bdevperf=$rootdir/build/examples/bdevperf

source "$curdir/mallocs.conf"
$bdevperf --json <(gen_malloc_ocf_json) -q 128 -o 4096 -w write -t 120 -r /var/tmp/spdk.sock &
bdev_perf_pid=$!
waitforlisten $bdev_perf_pid
sleep 1
$rpc_py bdev_ocf_get_stats MalCache1
kill -9 $bdev_perf_pid
wait $bdev_perf_pid || true
