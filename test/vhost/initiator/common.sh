#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))

if [[ $RUN_NIGHTLY -eq 1 ]]; then
        fio_rw=("write" "randwrite" "rw" "randrw")
else
        fio_rw=("randwrite")
fi

function run_host_fio() {
        local fio_bdev_conf=$1
        LD_PRELOAD=$PLUGIN_DIR/fio_plugin $fio_bin --ioengine=spdk_bdev \
                --runtime=10 $BASE_DIR/bdev.fio "$fio_bdev_conf" --spdk_mem=1024
        rm -f *.state
        rm -f $BASE_DIR/bdev.fio
}

function prepare_fio_job_for_guest() {
        local rw="$1"
        local fio_bdevs="$2"
        echo "[job_$rw]" >> $BASE_DIR/bdev.fio
        echo "stonewall" >> $BASE_DIR/bdev.fio
        echo "rw=$rw" >> $BASE_DIR/bdev.fio
        echo -n "filename=" >> $BASE_DIR/bdev.fio
        for b in $(echo $fio_bdevs | jq -r '.name'); do
                echo -n "$b:" >> $BASE_DIR/bdev.fio
        done
}

function prepare_fio_job_for_host() {
        local rw="$1"
        local fio_bdevs="$2"
        for b in $(echo $fio_bdevs | jq -r '.name'); do
                echo "[job_$rw_$b]" >> $BASE_DIR/bdev.fio
                echo "rw=$rw" >> $BASE_DIR/bdev.fio
                echo "filename=$b" >> $BASE_DIR/bdev.fio
        done
}

function define_fio_filename() {
        fio_bdevs="$1"
        echo -n "filename=" >> $BASE_DIR/bdev.fio
        for b in $(echo $fio_bdevs | jq -r '.name'); do
                echo -n "$b:" >> $BASE_DIR/bdev.fio
        done
}

function prepare_fio_job_4G() {
        rw="$1"
        fio_bdevs="$2"
        echo "size=1G" >> $BASE_DIR/bdev.fio
        echo "io_size=4G" >> $BASE_DIR/bdev.fio
        echo "offset=4G" >> $BASE_DIR/bdev.fio
        echo "[job_$rw]" >> $BASE_DIR/bdev.fio
        echo "stonewall" >> $BASE_DIR/bdev.fio
        echo "rw=$rw" >> $BASE_DIR/bdev.fio
        define_fio_filename "$fio_bdevs"
}

function prepare_fio_job_for_unmap() {
        local fio_bdevs="$1"
        define_fio_filename "$fio_bdevs"
        echo "" >> $BASE_DIR/bdev.fio
        echo "size=100m" >> $BASE_DIR/bdev.fio
        echo "io_size=400m" >> $BASE_DIR/bdev.fio

        # Check that sequential TRIM/UNMAP operations 'zeroes' disk space
        echo "[trim_sequential]" >> $BASE_DIR/bdev.fio
        echo "stonewall" >> $BASE_DIR/bdev.fio
        echo "rw=trim" >> $BASE_DIR/bdev.fio
        echo "trim_verify_zero=1" >> $BASE_DIR/bdev.fio

        # Check that random TRIM/UNMAP operations 'zeroes' disk space
        echo "[trim_random]" >> $BASE_DIR/bdev.fio
        echo "stonewall" >> $BASE_DIR/bdev.fio
        echo "rw=randtrim" >> $BASE_DIR/bdev.fio
        echo "trim_verify_zero=1" >> $BASE_DIR/bdev.fio

        # Check that after TRIM/UNMAP operation disk space can be used for read
        # by using write with verify (which implies reads)
        echo "[write]" >> $BASE_DIR/bdev.fio
        echo "stonewall" >> $BASE_DIR/bdev.fio
        echo "rw=write" >> $BASE_DIR/bdev.fio
}

function start_and_prepare_vm() {
        local os="$os_image"
        local test_type="spdk_vhost_scsi"
        local disk="Nvme0n1"
        local force_vm_num="0"
        local vm_num="0"
        local fio_bin="root/fio_src/fio"
        local setup_cmd="$COMMON_DIR/vm_setup.sh $x --work-dir=$TEST_DIR --test-type=$test_type"
        setup_cmd+=" --os=$os --disk=$disk -f $force_vm_num --memory=4096 --queue_num=16"
        $setup_cmd

        echo "used_vms: $vm_num"
        $COMMON_DIR/vm_run.sh $x --work-dir=$TEST_DIR $vm_num
        vm_wait_for_boot 600 $vm_num
        vm_scp $vm_num -r $ROOT_DIR "127.0.0.1:/root/spdk"
        vm_ssh $vm_num " cd spdk ; make clean ; ./configure --with-fio=/root/fio_src ; make -j3"
        vm_dir=$VM_BASE_DIR/$vm_num
        qemu_mask_param="VM_${vm_num}_qemu_mask"
        host_name="VM-$vm_num-${!qemu_mask_param}"
        echo "INFO: Setting up hostname: $host_name"
        vm_ssh $vm_num "hostname $host_name"
        vm_start_fio_server --fio-bin=$fio_bin $vm_num
}

function run_guest_bdevio() {
        local vm_num="0"
        local conf_file="$BASE_DIR/bdevvm.conf"
        vm_scp $vm_num  $conf_file "127.0.0.1:/root/bdev.conf"
        timing_enter bounds
        vm_ssh $vm_num "/root/spdk/scripts/setup.sh ; /root/spdk/test/lib/bdev/bdevio/bdevio /root/bdev.conf"
        timing_exit bounds
}

function run_guest_fio() {
        echo "INFO: Running fio jobs ..."
        local fio_bdev_conf=$1
        local vm_num="0"
        local fio_job="$BASE_DIR/bdev.fio"
        vm_scp $vm_num $fio_job "127.0.0.1:/root/bdev.fio"
        vm_ssh $vm_num "/root/spdk/test/vhost/initiator/guest_fio.sh $fio_bdev_conf"
        vm_ssh $vm_num "rm -f *.state"
}

function on_error_exit() {
        set +e
        echo "Error on $1 - $2"
        vm_shutdown_all
        spdk_vhost_kill
        print_backtrace
        exit 1
}