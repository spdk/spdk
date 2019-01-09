#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
source $curdir/common.sh

bdevperf=$rootdir/test/bdev/bdevperf/bdevperf

$bdevperf -c $curdir/mallocs.conf -q 128 -o 4096 -t 4 -w flush
$bdevperf -c $curdir/mallocs.conf -q 128 -o 4096 -t 4 -w unmap
