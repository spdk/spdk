set -xe

# Virtio SCSI and BLK hotremove tests using virtio devs

function run_spdk_fio() {
    fio_bdev --ioengine=spdk_bdev "$@" --spdk_mem=1024 --spdk_single_seg=1
}

$rpc_py vhost_create_scsi_controller naa.Nvme0n1_scsi0.0
$rpc_py vhost_scsi_controller_add_target naa.Nvme0n1_scsi0.0 0 Nvme0n1p0
$rpc_py vhost_create_blk_controller naa.Nvme0n1_blk0.0 Nvme0n1p1
vbdevs=$(discover_bdevs $rootdir $testdir/bdev.conf)
virtio_bdevs=$(jq -r '[.[].name] | join(":")' <<< $vbdevs)

OLD_IFS=$IFS
IFS=':' read -ra vbdevs <<< "$virtio_bdevs"
IFS=$OLD_IFS
run_spdk_fio $testdir/fio_jobs/virtio_integrity.job --filename=${vbdevs[0]} --spdk_conf=$testdir/bdev.conf &
fio_pid1=$!
run_spdk_fio $testdir/fio_jobs/virtio_integrity.job --filename=${vbdevs[1]} --spdk_conf=$testdir/bdev.conf &
fio_pid2=$!
sleep 5
traddr=""
get_traddr "Nvme0"
delete_nvme "Nvme0"
retcode_fio1=0
wait_for_finish $fio_pid1 || retcode_fio1=$?
retcode_fio2=0
wait_for_finish $fio_pid2 || retcode_fio2=$?
check_fio_retcode "Virtio scsi hotremove" 1 $retcode_fio1
check_fio_retcode "Virtio blk hotremove" 1 $retcode_fio2

$rpc_py remove_vhost_controller naa.Nvme0n1_scsi0.0
$rpc_py remove_vhost_controller naa.Nvme0n1_blk0.0
