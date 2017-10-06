#!/usr/bin/env bash

set -x

testdir=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $testdir/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $testdir/../../../../ && pwd)"
rootdir=$(readlink -f $testdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin
rpc_py="$rootdir/scripts/rpc.py"

guest_bdevs=""

function run_host_fio() {
        LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev --iodepth=16 --bs=192k --runtime=10 $testdir/bdev.fio "$2" --spdk_mem=1024
        fio_status=$?
        if [ $fio_status != $3 ]; then
                echo "Test $1 failed."
                spdk_vhost_kill
                vm_shutdown_all
                exit 1
        fi
}

function prepare_fio_job() {
        rw="$1"
        fio_bdevs="$2"
        if [ $rw == "read" ] || [ $rw == "randread" ]; then
                echo "size=100m" >> $testdir/bdev.fio
                echo "io_size=400m" >> $testdir/bdev.fio
        fi
        if [ $rw == "read" ] || [ $rw == "randread" ]; then
                echo "[job_write]" >> $testdir/bdev.fio
                echo "stonewall" >> $testdir/bdev.fio
                echo "rw=write" >> $testdir/bdev.fio
                echo "do_verify=0" >> $testdir/bdev.fio
                echo -n "filename=" >> $testdir/bdev.fio
                for b in $(echo $fio_bdevs | jq -r '.name'); do
                        echo -n "$b:" >> $testdir/bdev.fio
                done
                echo "" >> $testdir/bdev.fio
        fi
        echo "[job_$rw]" >> $testdir/bdev.fio
        echo "stonewall" >> $testdir/bdev.fio
        echo "rw=$rw" >> $testdir/bdev.fio
        echo -n "filename=" >> $testdir/bdev.fio
        for b in $(echo $fio_bdevs | jq -r '.name'); do
                echo -n "$b:" >> $testdir/bdev.fio
        done
}

function prepare_fio_job_for_unmap() {
	fio_bdevs="$1"
        echo -n "filename=" >> $testdir/bdev.fio
        for b in $(echo $fio_bdevs | jq -r 'select(.supported_io_types.unmap == true) | .name'); do
                echo -n "$b:" >> $testdir/bdev.fio
        done
        echo "" >> $testdir/bdev.fio
        echo "size=100m" >> $testdir/bdev.fio
        echo "io_size=400m" >> $testdir/bdev.fio
        echo "offset=8G" >> $testdir/bdev.fio
        echo "[job_wr1]" >> $testdir/bdev.fio
        echo "stonewall" >> $testdir/bdev.fio
        echo "rw=write" >> $testdir/bdev.fio
        echo "[job_rd1]" >> $testdir/bdev.fio
        echo "stonewall" >> $testdir/bdev.fio
        echo "rw=read" >> $testdir/bdev.fio
        echo "[job_tr1]" >> $testdir/bdev.fio
        echo "stonewall" >> $testdir/bdev.fio
        echo "rw=trim" >> $testdir/bdev.fio
        echo "[job_rd2]" >> $testdir/bdev.fio
        echo "stonewall" >> $testdir/bdev.fio
        echo "rw=read" >> $testdir/bdev.fio
        echo "verify_pattern=0x0000" >> $testdir/bdev.fio
        echo "do_verify=1" >> $testdir/bdev.fio
}

function start_and_prepare_vm() {
        os="/home/sys_sgsw/vhost_vm_image.qcow2"
        test_type="spdk_vhost_scsi"
        disk="Nvme0n1"
        force_vm_num="0"
        vm_num="0"
        os_mode="original"
        setup_cmd="$COMMON_DIR/vm_setup.sh $x --work-dir=$TEST_DIR --test-type=$test_type"
        setup_cmd+=" --os=$os --disk=$disk -f $force_vm_num --os-mode=$os_mode --memory=4096"
        $setup_cmd

        echo "used_vms: $vm_num"
        $COMMON_DIR/vm_run.sh $x --work-dir=$TEST_DIR $vm_num
        vm_wait_for_boot 600 $vm_num
        vm_scp $vm_num -r $rootdir "127.0.0.1:/root/spdk"
        vm_ssh $vm_num " cd spdk ; git checkout -- . ; git checkout master "
        vm_ssh $vm_num " cd spdk ; make clean ; ./configure --with-fio=/root/fio_src ; make "
}

function run_guest_bdevio() {
        vm_num="0"
        conf_file="$testdir/bdevvm.conf"
        vm_scp $vm_num  $conf_file "127.0.0.1:/root/bdev.conf"
        timing_enter bounds
        vm_ssh $vm_num "./spdk/scripts/setup.sh ; /root/spdk/test/lib/bdev/bdevio/bdevio /root/bdev.conf"
        timing_exit bounds
}

function run_guest_fio() {
        echo "INFO: Running fio jobs ..."
        vm_num="0"
        readonly=""
        fio_job="$testdir/bdev.fio"
        conf_file="$testdir/bdevvm.conf"
        run_fio="LD_PRELOAD=/root/spdk/examples/bdev/fio_plugin/fio_plugin /root/fio_src/fio --ioengine=spdk_bdev"
        run_fio+=" --iodepth=16 --bs=192k --runtime=10 /root/bdev.fio $2 --spdk_mem=1024"

        vm_dir=$VM_BASE_DIR/$vm_num
        qemu_mask_param="VM_${vm_num}_qemu_mask"
        host_name="VM-$vm_num-${!qemu_mask_param}"
        echo "INFO: Setting up hostname: $host_name"
        vm_ssh $vm_num "hostname $host_name"
        vm_start_fio_server $fio_bin $readonly $vm_num
        vm_scp $vm_num $fio_job "127.0.0.1:/root/bdev.fio"
        disc_bdevs=" ./spdk/scripts/setup.sh ; . ./spdk/scripts/autotest_common.sh ; "
        disc_bdevs+='bdevs=$(discover_bdevs /root/spdk /root/bdev.conf | jq -r '"'"'.[] | select(.claimed == false)'"'"') ; '
        disc_bdevs+='virtio_bdevs="" ; for b in $(echo $bdevs | jq -r '"'"'.name'"'"') ; do virtio_bdevs+="$b:" ; done ; '
        disc_bdevs+='sed -i "s|filename=|filename=$virtio_bdevs|g" /root/bdev.fio'
        vm_ssh $vm_num "$disc_bdevs"
        vm_ssh $vm_num "cat /root/bdev.fio"
        vm_ssh $vm_num $run_fio
        fio_status=$?
        if [ $fio_status != $3 ]; then
                vm_ssh $vm_num "ls"
                echo "Test $1 failed."
                spdk_vhost_kill
                vm_shutdown_all
                exit 1
        fi
}

source $rootdir/test/vhost/common/common.sh
$rootdir/scripts/gen_nvme.sh
spdk_vhost_run $testdir
$rpc_py construct_malloc_bdev 128 512
$rpc_py construct_malloc_bdev 128 4096
$rpc_py add_vhost_scsi_lun naa.Nvme0n1.0 0 Nvme0n1
$rpc_py add_vhost_scsi_lun vhost.1 0 Malloc0
$rpc_py add_vhost_scsi_lun vhost.2 0 Malloc1
$rpc_py get_bdevs
bdevs=$($rpc_py get_bdevs | jq -r '.[] | .name')

for bdev in $bdevs; do
        timing_enter bdev

        cp $testdir/bdev.conf.in $testdir/bdev.conf
        if [ $bdev == "Nvme0n1" ]; then
                sed -i "s|/tmp/vhost.0|$rootdir/../vhost/naa.Nvme0n1.0|g" $testdir/bdev.conf
        elif [ $bdev == "Malloc0" ]; then
                sed -i "s|/tmp/vhost.0|$rootdir/../vhost/vhost.1|g" $testdir/bdev.conf
        else
                sed -i "s|/tmp/vhost.0|$rootdir/../vhost/vhost.2|g" $testdir/bdev.conf
        fi

        timing_enter bounds
        $rootdir/test/lib/bdev/bdevio/bdevio $testdir/bdev.conf
        timing_exit bounds

        timing_enter bdev_svc
        vbdevs=$(discover_bdevs $rootdir $testdir/bdev.conf 5261 | jq -r '.[] | select(.claimed == false)')
        timing_exit bdev_svc
        if [ -d /usr/src/fio ]; then
                timing_enter fio
                if [ $RUN_NIGHTLY -eq 1 ]; then
                        fio_rw=("write" "read" "randwrite" "randread" "rw" "randrw")
                else
                        fio_rw=("write" "read")
                fi
                if [ $bdev == "Nvme0n1" ]; then
                        start_and_prepare_vm
                        run_guest_bdevio
                        for rw in "${fio_rw[@]}"; do
                                cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                                prepare_fio_job "$rw" ""
                                run_guest_fio "Guest $bdev" --spdk_conf=/root/bdev.conf 0
                                rm -f *.state
                                rm -f $testdir/bdev.fio
                        done
                        # Guest test for unmap
                        cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                        prepare_fio_job_for_unmap ""
                        run_guest_fio "Guest $bdev" --spdk_conf=/root/bdev.conf 0
                        rm -f *.state
                        rm -f $testdir/bdev.fio
                        vm_shutdown_all
                fi
                for rw in "${fio_rw[@]}"; do
                        timing_enter fio_rw_verify
                        cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                        prepare_fio_job "$rw" "$vbdevs"
                        run_host_fio "Host unamp $bdev" --spdk_conf=$testdir/bdev.conf

                        rm -f *.state
                        rm -f $testdir/bdev.fio
                        timing_exit fio_rw_verify
                done
                #Host test for unmap
                cp $testdir/../common/fio_jobs/default_initiator.job $testdir/bdev.fio
                prepare_fio_job_for_unmap "$vbdevs"
                run_host_fio "Host unmap $bdev" --spdk_conf=$testdir/bdev.conf 0
                rm -f *.state
                rm -f $testdir/bdev.fio

                timing_exit fio
        fi

        rm -f $testdir/bdev.conf
        timing_exit bdev
done
vm_shutdown_all
spdk_vhost_kill
