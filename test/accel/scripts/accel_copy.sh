#!/usr/bin/bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2025 StarWind Software, Inc.
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)

# default test parameters
module=software
size=65536
mask=0x1
nbufs=1
queue=64

[[ $1 != "" ]] && module=$1
[[ $2 != "" ]] && size=$((1024 * $2))
[[ $3 != "" ]] && mask=$3
[[ $4 != "" ]] && [[ $4 -gt 0 && $4 -le 16 ]] && nbufs=$4
[[ $5 != "" ]] && [[ $5 -gt 0 && $5 -lt 4096 ]] && queue=$5
[[ $6 != "" ]] && option=$6

echo "COPY with $module (block $size, core_mask $mask, nbufs $nbufs, queue $queue $option)..."

if [ $module = "cuda" ]; then
	$rootdir/build/examples/accel_perf -m $mask -q $queue -w copy -o $size -C $nbufs $option -r /var/tmp/spdk.sock --wait-for-rpc &
	spdk_pid=$!
	waitforlisten $spdk_pid
	sleep 1

	$rootdir/scripts/rpc.py cuda_scan_accel_module
	$rootdir/scripts/rpc.py accel_assign_opc -o copy -m accel_cuda
	$rootdir/scripts/rpc.py framework_start_init

	echo "Waiting for accel_perf PID" $spdk_pid
	wait $spdk_pid
else
	$rootdir/build/examples/accel_perf -m $mask -q $queue -w copy -o $size -C $nbufs $option
fi
