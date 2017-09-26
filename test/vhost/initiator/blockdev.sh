#!/usr/bin/env bash

if [[ $RUN_NIGHTLY -eq 1 ]]; then
        fio_rw=("write" "randwrite" "rw" "randrw")
else
        fio_rw=("randwrite")
fi

function usage() {
        [[ ! -z $2 ]] && ( echo "$2"; echo ""; )
        echo "Script for running vhost initiator tests."
        echo "Usage: $(basename $1) [-h|--help] [--os]"
        echo "    --os=PATH      Path to VM image used in these tests"
        echo "    --fiobin=PATH  Path to fio binary on host"
        exit 0
}

os_image="/home/sys_sgsw/vhost_vm_image.qcow2"
fio_bin="/usr/src/fio/fio"
while getopts 'h-:' optchar; do
        case "$optchar" in
                -)
                case "$OPTARG" in
                        help) usage $0 ;;
                        os=*) os_image="${OPTARG#*=}" ;;
                        fiobin=*) fio_bin="${OPTARG#*=}" ;;
                        *) usage $0 echo "Invalid argument '$OPTARG'" ;;
                esac
                ;;
        h) usage $0 ;;
        *) usage $0 "Invalid argument '$optchar'" ;;
        esac
done

set -xe
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)
PLUGIN_DIR=$ROOT_DIR/examples/bdev/fio_plugin
RPC_PY="$ROOT_DIR/scripts/rpc.py"

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

source $ROOT_DIR/test/vhost/common/common.sh
$ROOT_DIR/scripts/gen_nvme.sh
spdk_vhost_run $BASE_DIR
trap 'on_error_exit ${FUNCNAME} - ${LINENO}' ERR
$RPC_PY construct_malloc_bdev 128 512
$RPC_PY construct_malloc_bdev 128 4096
$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.0 0 Nvme0n1
$RPC_PY add_vhost_scsi_lun naa.Malloc0.1 0 Malloc0
$RPC_PY add_vhost_scsi_lun naa.Malloc1.2 0 Malloc1
$RPC_PY get_bdevs

timing_enter bdev

cp $BASE_DIR/bdev.conf.in $BASE_DIR/bdev.conf
sed -i "s|/tmp/vhost.0|$ROOT_DIR/../vhost/naa.Nvme0n1.0|g" $BASE_DIR/bdev.conf
sed -i "s|/tmp/vhost.1|$ROOT_DIR/../vhost/naa.Malloc0.1|g" $BASE_DIR/bdev.conf
sed -i "s|/tmp/vhost.2|$ROOT_DIR/../vhost/naa.Malloc1.2|g" $BASE_DIR/bdev.conf

#Run bdevio tests on host
timing_enter bdevio_test
$ROOT_DIR/test/lib/bdev/bdevio/bdevio $BASE_DIR/bdev.conf
timing_exit bdevio_test

#Get bdevs for vhost initiator
timing_enter bdev_svc
vbdevs=$(discover_bdevs $ROOT_DIR $BASE_DIR/bdev.conf 5261 | jq -r '.[] | select(.claimed == false)')
timing_exit bdev_svc
if [ -e $fio_bin ]; then
        timing_enter fio_tests

        #Guest test
        start_and_prepare_vm
        #Run bdevio tests on guest
        run_guest_bdevio
        timing_enter fio_guest
        for rw in "${fio_rw[@]}"; do
                cp $BASE_DIR/../common/fio_jobs/default_initiator.job $BASE_DIR/bdev.fio
                prepare_fio_job_for_guest "$rw" ""
                echo "INFO: Guest test for Nvme0n1 - $rw"
                run_guest_fio --spdk_conf=/root/bdev.conf
                rm -f $BASE_DIR/bdev.fio
        done
        timing_enter fio_guest
        vm_shutdown_all

        #Host test
        timing_enter fio_host
        for rw in "${fio_rw[@]}"; do
                cp $BASE_DIR/../common/fio_jobs/default_initiator.job $BASE_DIR/bdev.fio
                prepare_fio_job_for_host "$rw" "$vbdevs"
                echo "INFO: Host test for all bdevs - $rw"
                run_host_fio --spdk_conf=$BASE_DIR/bdev.conf
                rm -f $BASE_DIR/bdev.fio
        done
        timing_exit fio_host

        #Host test for unmap
        timing_enter fio_host_unmap
        for vbdev in $(echo $vbdevs | jq -r '.name'); do
                cp $BASE_DIR/../common/fio_jobs/default_initiator.job $BASE_DIR/bdev.fio
                virtio_bdevs=$(echo $vbdevs | jq -r ". | select(.name == \"$vbdev\")")
                prepare_fio_job_for_unmap "$virtio_bdevs"
                echo "INFO: Host unmap test for $vbdev"
                run_host_fio  --spdk_conf=$BASE_DIR/bdev.conf
                rm -f $BASE_DIR/bdev.fio
        done
        timing_exit fio_host_unmap

        #Host test for +4G
        timing_enter fio_4G_rw_verify
        for rw in "${fio_rw[@]}"; do
                cp $BASE_DIR/../common/fio_jobs/default_initiator.job $BASE_DIR/bdev.fio
                virtio_nvme_bdev=$(echo $vbdevs | jq -r '. | select(.name == "VirtioScsi0t0")')
                prepare_fio_job_4G "$rw" "$virtio_nvme_bdev" "bdev.fio"
                echo "INFO: Host +4G test - $rw"
                run_host_fio --spdk_conf=$BASE_DIR/bdev.conf
                rm -f $BASE_DIR/bdev.fio
        done
        timing_exit fio_4G_rw_verify

        timing_exit fio_tests
else
        echo "Fio $fio_bin on host does not exist"
        exit 1
fi

rm -f $BASE_DIR/bdev.conf
timing_exit bdev
spdk_vhost_kill
