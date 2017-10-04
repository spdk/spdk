#!/usr/bin/env bash

set -x

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin

function format_disk_512() {
        $rootdir/scripts/setup.sh reset
        sleep 2
        last_nvme_disk=$( sudo nvme list | tail -n 1 )
        last_nvme_disk="$( cut -d ' ' -f 1 <<< "$last_nvme_disk" )"
        nvme format -l 0 $last_nvme_disk
        NRHUGE=8 $rootdir/scripts/setup.sh
        sleep 2
}

function format_disk_4096() {
        $rootdir/scripts/setup.sh reset
        sleep 2
        last_nvme_disk=$( sudo nvme list | tail -n 1 )
        last_nvme_disk="$( cut -d ' ' -f 1 <<< "$last_nvme_disk" )"
        nvme format -l 3 $last_nvme_disk
        NRHUGE=8 $rootdir/scripts/setup.sh
        sleep 2
}

function run_fio()
{
        LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev --iodepth=128 --bs=4k --runtime=10 $testdir/bdev.fio "$@"
        fio_status=$?
        if [ $fio_status != 0 ]; then
                spdk_vhost_kill
                exit 1
        fi
}

source $rootdir/test/vhost/common/common.sh
$rootdir/scripts/gen_nvme.sh
sudo NRHUGE=8 $rootdir/scripts/setup.sh
sleep 4
spdk_vhost_run $testdir

for block_size in ; do #512 4096; do
        if [ $RUN_NIGHTLY -eq 1 ]; then
                format_disk_${block_size}
        fi
        $rootdir/scripts/rpc.py construct_malloc_bdev 128 ${block_size}
        $rootdir/scripts/rpc.py get_bdevs
        if [ $block_size == 512 ]; then
        	$rootdir/scripts/rpc.py add_vhost_scsi_lun vhost.1 0 Malloc0
        else
                $rootdir/scripts/rpc.py add_vhost_scsi_lun vhost.1 0 Malloc1
        fi
        $rootdir/scripts/rpc.py add_vhost_scsi_lun vhost.0 0 Nvme0n1
        for bdev_type in "nvme" "malloc"; do
                timing_enter bdev

                cp $testdir/bdev.conf.in $testdir/bdev.conf
                if [ $bdev_type == "malloc" ]; then
                        sed -i "s|/tmp/vhost.0|$rootdir/../vhost/vhost.1|g" $testdir/bdev.conf
                elif [ $bdev_type == "nvme" ]; then
                        sed -i "s|/tmp/vhost.0|$rootdir/../vhost/vhost.0|g" $testdir/bdev.conf
                fi

                timing_enter bounds
                $rootdir/test/lib/bdev/bdevio/bdevio $testdir/bdev.conf
                timing_exit bounds

                timing_enter bdev_svc
                bdevs=$(discover_bdevs $rootdir $testdir/bdev.conf 5261 | jq -r '.[] | select(.claimed == false)')
                timing_exit bdev_svc

                if [ -d /usr/src/fio ]; then
	                timing_enter fio
                        if [ $RUN_NIGHTLY -eq 1 ]; then
                                fio_rw=("write" "read" "randwrite" "randread" "rw" "randrw")
                        else
                                fio_rw=("write" "read")
                        fi
                        for rw in $fio_rw; do
	                        timing_enter fio_rw_verify
                                cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                                if [ $rw == "read" ] || [ $rw == "randread" ]; then
                                        echo "size=100m" >> $testdir/bdev.fio
                                        echo "io_size=400m" >> $testdir/bdev.fio
                                        echo "[job_write]" >> $testdir/bdev.fio
                                        echo "stonewall" >> $testdir/bdev.fio
                                        echo "rw=write" >> $testdir/bdev.fio
                                        echo "do_verify=0" >> $testdir/bdev.fio
                                        echo -n "filename=" >> $testdir/bdev.fio
                                        for b in $(echo $bdevs | jq -r '.name'); do
                                                echo -n "$b:" >> $testdir/bdev.fio
                                        done
                                        echo "" >> $testdir/bdev.fio
                                fi
                                echo "[job_$rw]" >> $testdir/bdev.fio
                                echo "stonewall" >> $testdir/bdev.fio
                                echo "rw=$rw" >> $testdir/bdev.fio
                                echo -n "filename=" >> $testdir/bdev.fio
	                        for b in $(echo $bdevs | jq -r '.name'); do
	                        	echo -n "$b:" >> $testdir/bdev.fio
                                done

                                cat $testdir/bdev.fio
	                        run_fio --spdk_conf=$testdir/bdev.conf

	                        rm -f *.state
	                        rm -f $testdir/bdev.fio
	                        timing_exit fio_rw_verify
                        done

                        timing_exit fio
                fi

                rm -f $testdir/bdev.conf
                timing_exit bdev
        done
        $rootdir/scripts/rpc.py remove_vhost_scsi_dev vhost.1 0
        $rootdir/scripts/rpc.py remove_vhost_scsi_dev vhost.0 0
done

$rootdir/scripts/rpc.py add_vhost_scsi_lun vhost.0 0 Nvme0n1
cp $testdir/vhost.conf.mq $testdir/vhost.conf
sed -i "s|/tmp/vhost.0|$rootdir/../vhost/vhost.0|g" $testdir/vhost.conf
sed -i "s|/tmp/vhost.1|$rootdir/../vhost/vhost.1|g" $testdir/vhost.conf
$rootdir/app/vhost/vhost -m 0x18 -p 0 -N -S $rootdir/../vhost -c $testdir/vhost.conf &
vhost_pid=$!
waitforlisten "$vhost_pid" 5261
$rootdir/scripts/rpc.py -p 5261 get_bdevs
$rootdir/scripts/rpc.py -p 5261 add_vhost_scsi_lun naa.Virtio0p0.0 0 Virtio0p0
$rootdir/scripts/rpc.py -p 5261 add_vhost_scsi_lun naa.Virtio0p1.0 0 Virtio0p1
setup_cmd="$testdir/../common/vm_setup.sh $x --work-dir=$rootdir/../ --test-type=spdk_vhost_scsi "
setup_cmd+="-f 0 --os=/home/sys_sgsw/vhost_vm_image.qcow2 --disk=Virtio0p0:Virtio0p1"
$setup_cmd
$testdir/../common/vm_run.sh --work-dir=$rootdir/../ "0"
vm_wait_for_boot 600 "0"
vm_dir=$BASEDIR/$vm_num

qemu_mask_param="VM_0_qemu_mask"
host_name="VM-0-${!qemu_mask_param}"
echo "INFO: Setting up hostname: $host_name"
vm_ssh "0" "hostname $host_name"
vm_start_fio_server --fio-bin=/home/sys_sgsw/fio_ubuntu "0"
rm $testdir/vhost.conf
vm_kill_all
/bin/kill -KILL $vhost_pid
spdk_vhost_kill
