#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"

function get_traddr() {
	local nvme_name=$1
	local nvme
	nvme="$( $rootdir/scripts/gen_nvme.sh )"
	while read -r line; do
		if [[ $line == *"TransportID"* ]] && [[ $line == *$nvme_name* ]]; then
			local word_array=($line)
			for word in "${word_array[@]}"; do
				if [[ $word == *"traddr"* ]]; then
					traddr=$( echo $word | sed 's/traddr://' | sed 's/"//' )
				fi
			done
		fi
	done <<< "$nvme"
}

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR

vhost_run 0
$rpc_py bdev_nvme_set_hotplug -e
$rpc_py bdev_split_create Nvme0n1 2
$rpc_py vhost_create_scsi_controller scsi_ctrl
$rpc_py vhost_scsi_controller_add_target scsi_ctrl 0 Nvme0n1p0
$rpc_py vhost_create_blk_controller blk_ctrl Nvme0n1p1

$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -g -q 128 -o 4096 -w verify -t 10 &
bdevperf_pid=$!
sleep 4
# Do hotremove
$rpc_py bdev_nvme_detach_controller "Nvme0"
waitforlisten $bdevperf_pid
if [[ $ret -ne 0 ]]; then
	error "Bdev perf ended with fail."
	exit 1
fi

get_traddr "Nvme0"
$rpc_py vhost_delete_controller blk_ctrl
$rpc_py bdev_nvme_attach_controller -b "HotInNvme1" -t PCIe -a "$traddr"
$rpc_py bdev_split_create HotInNvme1n1 2
$rpc_py vhost_scsi_controller_add_target scsi_ctrl 0 HotInNvme1n1p0
$rpc_py vhost_create_blk_controller blk_ctrl HotInNvme1n1p1
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -g -q 128 -o 4096 -w verify -t 5
if [[ $ret -ne 0 ]]; then
        error "Bdev perf ended with fail."
        exit 1
fi

$rpc_py vhost_scsi_controller_remove_target scsi_ctrl 0
$rpc_py vhost_delete_controller scsi_ctrl
$rpc_py vhost_delete_controller blk_ctrl
vhost_kill 0
