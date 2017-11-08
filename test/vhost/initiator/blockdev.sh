#!/usr/bin/env bash

set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)

PLUGIN_DIR=$ROOT_DIR/examples/bdev/fio_plugin
RPC_PY="$ROOT_DIR/scripts/rpc.py"

os_image="/home/sys_sgsw/vhost_vm_image.qcow2"
fio_bin="/usr/src/fio/fio"

function usage() {
        [[ ! -z $2 ]] && ( echo "$2"; echo ""; )
        echo "Script for running vhost initiator tests."
        echo "Usage: $(basename $1) [-h|--help] [--os]"
        echo "    --os=PATH      Path to VM image used in these tests"
        echo "    --fiobin=PATH  Path to fio binary on host"
        exit 0
}

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

set -x
source $ROOT_DIR/test/vhost/common/common.sh
trap 'on_error_exit ${FUNCNAME} - ${LINENO}' ERR

source $BASE_DIR/common.sh

if [ ! -e $fio_bin ]; then
	error "Invalid path of fio binary"
fi

if [[ $RUN_NIGHTLY -eq 1 ]]; then
        fio_rw=("write" "randwrite" "rw" "randrw")
else
        fio_rw=("randwrite")
fi

function create_bdev_config()
{
	$RPC_PY construct_malloc_bdev 128 512
	$RPC_PY construct_malloc_bdev 128 4096
	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.0 0 Nvme0n1
	$RPC_PY add_vhost_scsi_lun naa.Malloc0.1 0 Malloc0
	$RPC_PY add_vhost_scsi_lun naa.Malloc1.2 0 Malloc1
	$RPC_PY get_bdevs
	cp $BASE_DIR/bdev.conf.in $BASE_DIR/bdev.conf
	sed -i "s|/tmp/vhost.0|$ROOT_DIR/../vhost/naa.Nvme0n1.0|g" $BASE_DIR/bdev.conf
	sed -i "s|/tmp/vhost.1|$ROOT_DIR/../vhost/naa.Malloc0.1|g" $BASE_DIR/bdev.conf
	sed -i "s|/tmp/vhost.2|$ROOT_DIR/../vhost/naa.Malloc1.2|g" $BASE_DIR/bdev.conf
	cat $BASE_DIR/bdev.conf
}

#timing_enter bdev

function bdevio_host_test()
{
#Run bdevio tests on host
	timing_enter bdevio_test
	$ROOT_DIR/test/lib/bdev/bdevio/bdevio $BASE_DIR/bdev.conf
	timing_exit bdevio_test
}

function bdevio_and_fio_guest_test()
{
#	timing_enter fio_tests
	start_and_prepare_vm
	
    timing_enter fio_guest
	run_guest_bdevio
	timing_enter fio_guest

	for rw in "${fio_rw[@]}"; do
		cp $BASE_DIR/../common/fio_jobs/default_initiator.job $BASE_DIR/bdev.fio
		prepare_fio_job_for_guest "$rw" ""
		echo "INFO: Guest test for Nvme0n1 - $rw"
		run_guest_fio --spdk_conf=/root/bdev.conf
		rm -f $BASE_DIR/bdev.fio
	done

	timing_exit fio_guest
	vm_shutdown_all		    
}

function fio_host_test()
{
	timing_enter fio_host
	for rw in "${fio_rw[@]}"; do
		cp $BASE_DIR/../common/fio_jobs/default_initiator.job $BASE_DIR/bdev.fio
		prepare_fio_job_for_host "$rw" "$vbdevs"
		echo "INFO: Host test for all bdevs - $rw"
		run_host_fio --spdk_conf=$BASE_DIR/bdev.conf
		rm -f $BASE_DIR/bdev.fio
		done
	timing_exit fio_host
}

function fio_unmap_host_test()
{
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
}

function fio_4G_host_test()
{
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
}
#Get bdevs for vhost initiator

#        timing_enter fio_tests
#
#        #Guest test
#        start_and_prepare_vm
#        #Run bdevio tests on guest
#        run_guest_bdevio
#        timing_enter fio_guest
#        for rw in "${fio_rw[@]}"; do
#                cp $BASE_DIR/../common/fio_jobs/default_initiator.job $BASE_DIR/bdev.fio
#                prepare_fio_job_for_guest "$rw" ""
#                echo "INFO: Guest test for Nvme0n1 - $rw"
#                run_guest_fio --spdk_conf=/root/bdev.conf
#                rm -f $BASE_DIR/bdev.fio
#        done
#        timing_exit fio_guest
#        vm_shutdown_all
#
#        #Host test
#        timing_enter fio_host
#        for rw in "${fio_rw[@]}"; do
#                cp $BASE_DIR/../common/fio_jobs/default_initiator.job $BASE_DIR/bdev.fio
#                prepare_fio_job_for_host "$rw" "$vbdevs"
#                echo "INFO: Host test for all bdevs - $rw"
#                run_host_fio --spdk_conf=$BASE_DIR/bdev.conf
#                rm -f $BASE_DIR/bdev.fio
#        done
#        timing_exit fio_host
#
#        #Host test for unmap
#        timing_enter fio_host_unmap
#        for vbdev in $(echo $vbdevs | jq -r '.name'); do
#                cp $BASE_DIR/../common/fio_jobs/default_initiator.job $BASE_DIR/bdev.fio
#                virtio_bdevs=$(echo $vbdevs | jq -r ". | select(.name == \"$vbdev\")")
#                prepare_fio_job_for_unmap "$virtio_bdevs"
#                echo "INFO: Host unmap test for $vbdev"
#                run_host_fio  --spdk_conf=$BASE_DIR/bdev.conf
#                rm -f $BASE_DIR/bdev.fio
#        done
#        timing_exit fio_host_unmap
#
#        #Host test for +4G
#        timing_enter fio_4G_rw_verify
#        for rw in "${fio_rw[@]}"; do
#                cp $BASE_DIR/../common/fio_jobs/default_initiator.job $BASE_DIR/bdev.fio
#                virtio_nvme_bdev=$(echo $vbdevs | jq -r '. | select(.name == "VirtioScsi0t0")')
#                prepare_fio_job_4G "$rw" "$virtio_nvme_bdev" "bdev.fio"
#                echo "INFO: Host +4G test - $rw"
#                run_host_fio --spdk_conf=$BASE_DIR/bdev.conf
#                rm -f $BASE_DIR/bdev.fio
#        done
#        timing_exit fio_4G_rw_verify

#        timing_exit fio_tests

#$ROOT_DIR/scripts/gen_nvme.sh
spdk_vhost_run $BASE_DIR

create_bdev_config
bdevio_host_test

timing_enter bdev_svc
vbdevs=$(discover_bdevs $ROOT_DIR $BASE_DIR/bdev.conf 5261 | jq -r '.[] | select(.claimed == false)')
echo $vbdevs
timing_exit bdev_svc

bdevio_and_fio_guest_test
fio_unmap_host_test
fio_4G_host_test
rm -f $BASE_DIR/bdev.conf
timing_exit bdev
spdk_vhost_kill
