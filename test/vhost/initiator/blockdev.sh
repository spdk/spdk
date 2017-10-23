#!/usr/bin/env bash

set -x

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"
ROOTDIR=$(readlink -f $BASE_DIR/../../../)
PLUGINDIR=$ROOTDIR/examples/bdev/fio_plugin
RPC_PY="$ROOTDIR/scripts/rpc.py"
script_on_vm="0"
fio_bin="/usr/src/fio/fio"

RUN_NIGHTLY=1
if [ $RUN_NIGHTLY -eq 1 ]; then
        fio_rw=("write" "randwrite" "rw" "randrw")
else
        fio_rw=("randwrite")
fi

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Script for running vhost initiator tests."
	echo "Usage: $(basename $1) [-h|--help] [--os]"
	echo "-h, --help         Print help and exit"
	echo "    --os=PATH      Path to VM image used in these tests"
	echo "    --fiobin=PATH  Path to fio binary on host"
	echo "    --vm           Run on vm"
}

while getopts 'h-:' optchar; do
        echo "Optchar $optchar"
	case "$optchar" in
		-)
                echo "optArG $optARG"
		case "$OPTARG" in
			help) usage $0 && exit 0 ;;
			os=*) os_image="${OPTARG#*=}" ;;
			fiobin=*) fio_bin="${OPTARG#*=}" ;;
			vm=*) script_on_vm="${OPTARG#*=}" ;;
			*) usage $0 echo "Invalid argument '$OPTARG'" && exit 1 ;;
		esac
		;;
		h) usage $0 && exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'" && exit 1 ;;
	esac
done

function run_fio() {
        local test_message=$1
        local fio_spdk_conf=$2
        echo $test_message
        LD_PRELOAD=$PLUGINDIR/fio_plugin $fio_bin --ioengine=spdk_bdev \
                --runtime=10 $BASE_DIR/bdev.fio "$fio_spdk_conf" --spdk_mem=1024
}

function prepare_fio_job_for_guest() {
        local rw="$1"
        local fio_bdevs="$2"
        local fio_job_name="$3"
        if [ $rw == "read" ] || [ $rw == "randread" ]; then
                echo "size=100m" >> $BASE_DIR/$fio_job_name
                echo "io_size=400m" >> $BASE_DIR/$fio_job_name
        fi
        if [ $rw == "read" ] || [ $rw == "randread" ]; then
                echo "[job_write]" >> $BASE_DIR/$fio_job_name
                echo "stonewall" >> $BASE_DIR/$fio_job_name
                echo "rw=write" >> $BASE_DIR/$fio_job_name
                echo "do_verify=0" >> $BASE_DIR/$fio_job_name
                echo -n "filename=" >> $BASE_DIR/$fio_job_name
                for b in $(echo $fio_bdevs | jq -r '.name'); do
                        echo -n "$b:" >> $BASE_DIR/$fio_job_name
                done
                echo "" >> $BASE_DIR/$fio_job_name
        fi
        echo "[job_$rw]" >> $BASE_DIR/$fio_job_name
        echo "stonewall" >> $BASE_DIR/$fio_job_name
        echo "rw=$rw" >> $BASE_DIR/$fio_job_name
        echo -n "filename=" >> $BASE_DIR/$fio_job_name
        for b in $(echo $fio_bdevs | jq -r '.name'); do
                echo -n "$b:" >> $BASE_DIR/$fio_job_name
        done
}

function prepare_fio_job_for_host() {
        local rw="$1"
        local fio_bdevs="$2"
        local fio_job_name="$3"
        if [ $rw == "read" ] || [ $rw == "randread" ]; then
                echo "size=100m" >> $BASE_DIR/$fio_job_name
                echo "io_size=400m" >> $BASE_DIR/$fio_job_name
        fi
        if [ $rw == "read" ] || [ $rw == "randread" ]; then
                for b in $(echo $fio_bdevs | jq -r '.name'); do
                        echo "[job_write_$b]" >> $BASE_DIR/$fio_job_name
                        echo "rw=write" >> $BASE_DIR/$fio_job_name
                        echo "do_verify=0" >> $BASE_DIR/$fio_job_name
                        echo "filename=$b" >> $BASE_DIR/$fio_job_name
                done
        fi
        local i=0
        for b in $(echo $fio_bdevs | jq -r '.name'); do
                echo "[job_${rw}_$b]" >> $BASE_DIR/$fio_job_name
                if [ $i == 0 ]; then
                        echo "stonewall" >> $BASE_DIR/$fio_job_name
                fi
                echo "rw=$rw" >> $BASE_DIR/$fio_job_name
                echo "filename=$b" >> $BASE_DIR/$fio_job_name
                i=$((i+1))
        done
}

function prepare_fio_job_4G() {
        rw="$1"
        fio_bdevs="$2"
        echo "size=1G" >> $testdir/bdev.fio
        echo "io_size=4G" >> $testdir/bdev.fio
        echo "offset=4G" >> $testdir/bdev.fio
        echo "[job_$rw]" >> $testdir/bdev.fio
        echo "stonewall" >> $testdir/bdev.fio
        echo "rw=$rw" >> $testdir/bdev.fio
        echo -n "filename=" >> $testdir/bdev.fio
        for b in $(echo $fio_bdevs | jq -r '.name'); do
                echo -n "$b:" >> $testdir/bdev.fio
        done
}

function prepare_fio_job_for_unmap() {
        local fio_bdevs="$1"
        local fio_job_name="$2"
        echo -n "filename=" >> $BASE_DIR/$fio_job_name
        for b in $(echo $fio_bdevs | jq -r '.name'); do
                echo -n "$b:" >> $BASE_DIR/$fio_job_name
        done
        echo "" >> $BASE_DIR/$fio_job_name
        echo "size=100m" >> $BASE_DIR/$fio_job_name
        echo "io_size=400m" >> $BASE_DIR/$fio_job_name

        # Check that sequential TRIM/UNMAP operations 'zeroes' disk space
        echo "[trim_sequential]" >> $BASE_DIR/$fio_job_name
        echo "stonewall" >> $BASE_DIR/$fio_job_name
        echo "rw=trim" >> $BASE_DIR/$fio_job_name
        echo "trim_verify_zero=1" >> $BASE_DIR/$fio_job_name

        # Check that random TRIM/UNMAP operations 'zeroes' disk space
        echo "[trim_random]" >> $BASE_DIR/$fio_job_name
        echo "stonewall" >> $BASE_DIR/$fio_job_name
        echo "rw=randtrim" >> $BASE_DIR/$fio_job_name
        echo "trim_verify_zero=1" >> $BASE_DIR/$fio_job_name

        # Check that after TRIM/UNMAP operation disk space can be used for read
        # by using write with verify (which implies reads)
        echo "[write]" >> $BASE_DIR/$fio_job_name
        echo "stonewall" >> $BASE_DIR/$fio_job_name
        echo "rw=write" >> $BASE_DIR/$fio_job_name
}

function prepare_fio_job_4G() {
        local rw="$1"
        local fio_bdevs="$2"
        local fio_job_name="$3"
        echo "size=1G" >> $BASE_DIR/$fio_job_name
        #echo "io_size=4G" >> $BASE_DIR/$fio_job_name
        echo "offset=4G" >> $BASE_DIR/$fio_job_name
        if [ $rw == "read" ] || [ $rw == "randread" ]; then
                echo "[job_write]" >> $BASE_DIR/$fio_job_name
                echo "stonewall" >> $BASE_DIR/$fio_job_name
                echo "rw=write" >> $BASE_DIR/$fio_job_name
                echo "do_verify=0" >> $BASE_DIR/$fio_job_name
                echo -n "filename=" >> $BASE_DIR/$fio_job_name
                for b in $(echo $fio_bdevs | jq -r '.name'); do
                    echo -n "$b:" >> $BASE_DIR/$fio_job_name
                done
                echo "" >> $BASE_DIR/$fio_job_name
        fi
        echo "[job_$rw]" >> $BASE_DIR/$fio_job_name
        echo "stonewall" >> $BASE_DIR/$fio_job_name
        echo "rw=$rw" >> $BASE_DIR/$fio_job_name
        echo -n "filename=" >> $BASE_DIR/$fio_job_name
        for b in $(echo $fio_bdevs | jq -r '.name'); do
                echo -n "$b:" >> $BASE_DIR/$fio_job_name
        done
}

function start_and_prepare_vm() {
        local os="/home/sys_sgsw/vhost_vm_image.qcow2"
        local test_type="spdk_vhost_scsi"
        local disk="Nvme0n1p0:Nvme0n1p1"
        local force_vm_num="0"
        local vm_num="0"
        local os_mode="original"
        local setup_cmd="$COMMON_DIR/vm_setup.sh $x --work-dir=$TEST_DIR --test-type=$test_type"
        setup_cmd+=" --os=$os --disk=$disk -f $force_vm_num --os-mode=$os_mode --memory=4096 --queue_num=20"
        $setup_cmd

        echo "used_vms: $vm_num"
        $COMMON_DIR/vm_run.sh $x --work-dir=$TEST_DIR $vm_num
        vm_wait_for_boot 600 $vm_num
        local vm_dir=$VM_BASE_DIR/$vm_num
        local qemu_mask_param="VM_${vm_num}_qemu_mask"
        local host_name="VM-$vm_num-${!qemu_mask_param}"
        echo "INFO: Setting up hostname: $host_name"
        vm_ssh $vm_num "hostname $host_name"
        vm_start_fio_server "--fio-bin=$fio_bin" $readonly $vm_num
        vm_scp $vm_num -r $ROOTDIR "127.0.0.1:/root/spdk"
        vm_ssh $vm_num " cd spdk ; make clean ; ./configure --with-fio=/root/fio_src ; make -j3"
}

function on_error_exit() {
        set +e
        echo "Error on $1 - $2"
        vm_shutdown_all
        spdk_vhost_kill
        print_backtrace
        exit 1
}

#$ROOTDIR/scripts/gen_nvme.sh

echo "scrip on vm: $script_on_vm"
if [ $script_on_vm == "0" ]; then
        source $ROOTDIR/test/vhost/common/common.sh
        trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
	spdk_vhost_run $BASE_DIR
	$RPC_PY construct_malloc_bdev 128 512
	$RPC_PY construct_malloc_bdev 128 4096
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1p1.0 0 Nvme0n1p1
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1p1.0 1 Nvme0n1p2
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.2 0 Nvme0n1p3
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.3 0 Nvme0n1p4
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.3 1 Nvme0n1p5
	$RPC_PY add_vhost_scsi_lun naa.Malloc0.4 0 Malloc0
	$RPC_PY add_vhost_scsi_lun naa.Malloc1.5 0 Malloc1
	$RPC_PY get_bdevs
	bdevs=$($RPC_PY get_bdevs | jq -r '.[] | .name')

	timing_enter bdev

	cp $BASE_DIR/bdev.conf.in $BASE_DIR/bdev.conf
	sed -i "s|/tmp/vhost.0|$ROOTDIR/../vhost/naa.Nvme0n1.2|g" $BASE_DIR/bdev.conf
	sed -i "s|/tmp/vhost.1|$ROOTDIR/../vhost/naa.Malloc0.4|g" $BASE_DIR/bdev.conf
	sed -i "s|/tmp/vhost.2|$ROOTDIR/../vhost/naa.Malloc1.5|g" $BASE_DIR/bdev.conf

	cp $BASE_DIR/bdevms.conf.in $BASE_DIR/bdevms.conf
	sed -i "s|/tmp/vhost.0|$ROOTDIR/../vhost/naa.Nvme0n1.2|g" $BASE_DIR/bdevms.conf
	sed -i "s|/tmp/vhost.1|$ROOTDIR/../vhost/naa.Malloc0.4|g" $BASE_DIR/bdevms.conf

	cp $BASE_DIR/bdevmq.conf.in $BASE_DIR/bdevmq.conf
	sed -i "s|/tmp/vhost.0|$ROOTDIR/../vhost/naa.Nvme0n1.3|g" $BASE_DIR/bdevmq.conf
else
        $ROOTDIR/scripts/setup.sh
        source $ROOTDIR/scripts/autotest_common.sh
	cp $BASE_DIR/bdevvm.conf $BASE_DIR/bdev.conf
        cp $BASE_DIR/bdevvm.conf $BASE_DIR/bdevms.conf
        cp $BASE_DIR/bdevvm.conf $BASE_DIR/bdevmq.conf
fi


timing_enter bounds
$ROOTDIR/test/lib/bdev/bdevio/bdevio $BASE_DIR/bdev.conf
timing_exit bounds

timing_enter bdev_svc
if [ $script_on_vm == "0" ]; then
        vbdevs=$(discover_bdevs $ROOTDIR $BASE_DIR/bdev.conf 5261 | jq -r '.[] | select(.claimed == false)')
        msbdevs=$(discover_bdevs $ROOTDIR $BASE_DIR/bdevms.conf 5261 | jq -r '.[] | select(.claimed == false)')
        mqbdevs=$(discover_bdevs $ROOTDIR $BASE_DIR/bdevmq.conf 5261 | jq -r '.[] | select(.claimed == false)')
else
        vbdevs=$(discover_bdevs $ROOTDIR $BASE_DIR/bdev.conf | jq -r '.[] | select(.claimed == false)')
        msbdevs=$(discover_bdevs $ROOTDIR $BASE_DIR/bdevms.conf | jq -r '.[] | select(.claimed == false)')
        mqbdevs=$(discover_bdevs $ROOTDIR $BASE_DIR/bdevmq.conf | jq -r '.[] | select(.claimed == false)')
fi
timing_exit bdev_svc
if [ 1 -eq 1 ]; then
        timing_enter fio
        if [ $script_on_vm == "0" ]; then
                start_and_prepare_vm
                vm_ssh "0" "/root/spdk/test/vhost/initiator/blockdev.sh --fiobin=/root/fio_src/fio --vm=1"
                vm_shutdown_all
        fi

        #Test for host
        for rw in "${fio_rw[@]}"; do
                timing_enter fio_rw_verify
                cp $BASE_DIR/../common/fio_jobs/default_initiator.job $BASE_DIR/bdev.fio
                if [ $script_on_vm == "0" ]; then
                        prepare_fio_job_for_host "$rw" "$vbdevs" "bdev.fio"
                else
                        prepare_fio_job_for_guest "$rw" "$vbdevs" "bdev.fio"
                fi
                run_fio "Host $vbdevs" --spdk_conf=$BASE_DIR/bdev.conf

                rm -f *.state
                rm -f $BASE_DIR/bdev.fio
                timing_exit fio_rw_verify
        done

        #Host test for unmap
        i=0
        test_bdevs=($bdevs)
        for vbdev in $(echo $vbdevs | jq -r '.name'); do
                cp $BASE_DIR/../common/fio_jobs/default_initiator.job $BASE_DIR/bdev.fio
                virtio_bdevs=$(echo $vbdevs | jq -r ". | select(.name == \"$vbdev\")")
                prepare_fio_job_for_unmap "$virtio_bdevs" "bdev.fio"
                run_fio "Host unmap ${test_bdevs[i]}" --spdk_conf=$BASE_DIR/bdev.conf
                rm -f *.state
                rm -f $BASE_DIR/bdev.fio
                i=$((i+1))
        done

        #Host test for multiple socket
        cp $BASE_DIR/../common/fio_jobs/default_initiator.job $BASE_DIR/bdev.fio
        prepare_fio_job_for_host "write" "$msbdevs" "bdev.fio"
        run_fio "Host multiple socket" --spdk_conf=$BASE_DIR/bdevms.conf
        rm -f *.state
        rm -f $BASE_DIR/bdev.fio

        #Host test for multiqueue
        cp $BASE_DIR/../common/fio_jobs/default_initiator.job $BASE_DIR/bdev.fio
        prepare_fio_job_for_host "write" "$mqbdevs" "bdev.fio"
        run_fio "Host multiqueue" --spdk_conf=$BASE_DIR/bdevmq.conf
        rm -f *.state
        rm -f $BASE_DIR/bdev.fio

        #Host test for +4G
        for rw in "${fio_rw[@]}"; do
                timing_enter fio_4G_rw_verify
                cp $BASE_DIR/../common/fio_jobs/default_initiator.job $BASE_DIR/bdev.fio
                virtio_nvme_bdev=$(echo $vbdevs | jq -r '. | select(.name == "VirtioScsi0t0")')
                prepare_fio_job_4G "$rw" "$virtio_nvme_bdev" "bdev.fio"
                run_fio "Host +4G" --spdk_conf=$BASE_DIR/bdev.conf

                rm -f *.state
                rm -f $BASE_DIR/bdev.fio
                timing_exit fio_4G_rw_verify
        done

        timing_exit fio
fi
rm -f $BASE_DIR/bdev.conf
rm -f $BASE_DIR/bdevmq.conf
rm -f $BASE_DIR/bdevms.conf
timing_exit bdev
if [ $script_on_vm == "0" ]; then
        spdk_vhost_kill
fi
