set -xe

# Virtio SCSI and BLK hotremove tests using virtio devs

function run_spdk_fio() {
    fio_bdev --ioengine=spdk_bdev "$@" --spdk_mem=1024 --spdk_single_seg=1
}

$rpc_py construct_vhost_scsi_controller naa.Nvme0n1_scsi0.0
$rpc_py add_vhost_scsi_lun naa.Nvme0n1_scsi0.0 0 Nvme0n1p0
$rpc_py construct_vhost_blk_controller naa.Nvme0n1_blk0.0 Nvme0n1p1
vbdevs=$(discover_bdevs $rootdir $testdir/bdev.conf)
virtio_bdevs=$(jq -r '[.[].name] | join(":")' <<< $vbdevs)

IFS=':' read -ra vbdevs <<< "$virtio_bdevs"
run_spdk_fio $testdir/fio_jobs/virtio_integrity.job --filename=${vbdevs[0]} --spdk_conf=$testdir/bdev.conf &
local fio_pid1=$!
run_spdk_fio $testdir/fio_jobs/virtio_integrity.job --filename=${vbdevs[1]} --spdk_conf=$testdir/bdev.conf &
local fio_pid2=$!
sleep 3
traddr=""
get_traddr "Nvme0"
delete_nvme "Nvme0"
wait_for_finish $fio_pid1 || retcode_fio1=$?
wait_for_finish $fio_pid2 || retcode_fio2=$?
check_fio_retcode "Virtio scsi hotremove" 1 $?
check_fio_retcode "Virtio blk hotremove" 1 $?

$rpc_py remove_vhost_controller naa.Nvme0n1_scsi0.0
$rpc_py remove_vhost_controller naa.Nvme0n1_blk0.0
