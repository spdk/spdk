#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "${BASH_SOURCE[0]}"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

bdevperf=$rootdir/test/bdev/bdevperf/bdevperf
rpc_py="$rootdir/scripts/rpc.py"

source "$curdir/mallocs.conf"
$bdevperf --json <(gen_malloc_ocf_json) -q 128 -o 4096 -t 4 -w flush
$bdevperf --json <(gen_malloc_ocf_json) -q 128 -o 4096 -t 4 -w unmap
$bdevperf --json <(gen_malloc_ocf_json) -q 128 -o 4096 -t 4 -w write
