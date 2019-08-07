!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh
source $rootdir/test/vhost/hotplug/common.sh

function run_spdk_fio() {
	retcode_fio=0
	if ! fio_bdev $testdir/fio_jobs/virtio_integrity.job --ioengine=spdk_bdev --filename="$1" \
			--spdk_conf=$testdir/bdev.conf --spdk_mem=1024 --spdk_single_seg=1; then
		retcode_fio=1
	fi

	check_fio_retcode "$2" 1 $retcode_fio
}

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR

vhost_run 0
$rpc_py bdev_nvme_set_hotplug -e
$rpc_py bdev_split_create Nvme0n1 2
$rpc_py vhost_create_scsi_controller naa.Nvme0n1_scsi0.0
$rpc_py vhost_scsi_controller_add_target naa.Nvme0n1_scsi0.0 0 Nvme0n1p0
$rpc_py vhost_create_blk_controller naa.Nvme0n1_blk0.0 Nvme0n1p1
vbdevs=$(discover_bdevs $rootdir $testdir/bdev.conf)
virtio_bdevs=$(jq -r '[.[].name] | join(":")' <<< $vbdevs)

OLD_IFS=$IFS
IFS=':' read -ra vbdevs <<< "$virtio_bdevs"
IFS=$OLD_IFS
run_spdk_fio "${vbdevs[0]}" "Virtio blk hotremove" &
fio_pid1=$!
run_spdk_fio "${vbdevs[1]}" "Virtio scsi hotremove" &
fio_pid2=$!
sleep 5
delete_nvme "Nvme0"
wait_for_finish $fio_pid1
wait_for_finish $fio_pid2

#$rpc_py vhost_delete_controller naa.Nvme0n1_scsi0.0
#$rpc_py vhost_delete_controller naa.Nvme0n1_blk0.0
vhost_kill 0
