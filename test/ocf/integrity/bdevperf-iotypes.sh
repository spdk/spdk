#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "${BASH_SOURCE[0]}"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

bdevperf=$rootdir/test/bdev/bdevperf/bdevperf
rpc_py="$rootdir/scripts/rpc.py"

$bdevperf -c $curdir/mallocs.conf -q 128 -o 4096 -t 4 -w flush
$bdevperf -c $curdir/mallocs.conf -q 128 -o 4096 -t 4 -w unmap
$bdevperf -c $curdir/mallocs.conf -q 128 -o 4096 -t 4 -w write
