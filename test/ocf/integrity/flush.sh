#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation.
#  All rights reserved.

curdir=$(dirname $(readlink -f "${BASH_SOURCE[0]}"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

bdevperf=$rootdir/build/examples/bdevperf
rpc_py="$rootdir/scripts/rpc.py -s /var/tmp/spdk.sock"

check_flush_in_progress() {
	$rpc_py bdev_ocf_flush_status MalCache0 \
		| jq -e '.in_progress' > /dev/null
}

bdevperf_config() {
	local config

	config="$(
		cat <<- JSON
			{
			  "method": "bdev_malloc_create",
			  "params": {
				"name": "Malloc0",
				"num_blocks": 102400,
				"block_size": 512
			  }
			},
			{
			  "method": "bdev_malloc_create",
			  "params": {
				"name": "Malloc1",
				"num_blocks": 1024000,
				"block_size": 512
			  }
			},
			{
			  "method": "bdev_ocf_create",
			  "params": {
				"name": "MalCache0",
				"mode": "wb",
				"cache_line_size": 4,
				"cache_bdev_name": "Malloc0",
				"core_bdev_name": "Malloc1"
			  }
			}
		JSON
	)"

	jq . <<- JSON
		{
		  "subsystems": [
			{
			  "subsystem": "bdev",
			  "config": [
				$(
		IFS=","
		printf '%s\n' "$config"
		),
				{
				  "method": "bdev_wait_for_examine"
				}
			  ]
			}
		  ]
		}
	JSON
}

$bdevperf --json <(bdevperf_config) -q 128 -o 4096 -w write -t 120 -r /var/tmp/spdk.sock &
bdevperf_pid=$!
trap 'killprocess $bdevperf_pid' SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid
sleep 5

$rpc_py bdev_ocf_flush_start MalCache0
sleep 1

while check_flush_in_progress; do
	sleep 1
done
$rpc_py bdev_ocf_flush_status MalCache0 | jq -e '.status == 0'
