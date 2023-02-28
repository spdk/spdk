#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"
vhost_name="0"

bdev_conf=$(
	cat <<- JSON
		{
		"subsystems": [
			{
			"subsystem": "bdev",
			"config": [
				{
				"method": "bdev_virtio_attach_controller",
				"params": {
					"vq_count": 2,
					"traddr": "$(get_vhost_dir $vhost_name)/Malloc.0",
					"dev_type": "blk",
					"vq_size": 512,
					"name": "VirtioBlk0",
					"trtype": "user"
				}
				},
				{
				"method": "bdev_wait_for_examine"
				}
			]
			}
		]
		}
	JSON
)

function run_spdk_fio() {
	fio_bdev --ioengine=spdk_bdev \
		"$rootdir/test/vhost/common/fio_jobs/default_initiator.job" --runtime=10 --rw=randrw \
		--spdk_mem=1024 --spdk_single_seg=1 --verify_state_save=0 \
		--spdk_json_conf=<(echo "$bdev_conf") "$@"
}

vhosttestinit "--no_vm"

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR SIGTERM SIGABRT

vhost_run -n "$vhost_name"

$rpc_py bdev_malloc_create -b Malloc 124 4096
$rpc_py vhost_create_blk_controller Malloc.0 Malloc

run_spdk_fio --size=50% --offset=0 --filename=VirtioBlk0 &
run_fio_pid=$!
sleep 1
run_spdk_fio --size=50% --offset=50% --filename=VirtioBlk0
wait $run_fio_pid
vhost_kill 0

vhosttestfini
