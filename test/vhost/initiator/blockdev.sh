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

declare -A test_type
test_type=([unmap]=0 [4G]=1 [host]=2 [guest]=3)

fio_jobs_daily=("--section=fio_job_unmap_trim_random"\
	"--section=fio_job_4G_randwrite"\
	"--section=fio_job_host_randwrite"\
	"--section=fio_job_guest_randwrite")
fio_jobs_nightly=("--section=fio_job_unmap_trim_sequential --section=fio_job_unmap_trim_random --section=fio_job_unmap_write"\
	"--section=fio_job_4G_randwrite --section=fio_job_4G_randrw --section=fio_job_4G_write --section=fio_job_4G_rw"\
	"--section=fio_job_host_randrw --section=fio_job_host_randwrite --section=fio_job_host_rw --section=fio_job_host_write"\
	"--section=fio_job_guest_randwrite --section=fio_job_guest_randrw --section=fio_job_guest_write --section=fio_job_guest_rw")
fio_jobs=("${fio_jobs_daily[@]}")

if [[ $RUN_NIGHTLY -eq 1 ]]; then
	fio_jobs=("${fio_jobs_nightly[@]}")
fi

function usage() {
        [[ ! -z $2 ]] && ( echo "$2"; echo ""; )
        echo "Script for running vhost initiator tests."
        echo "Usage: $(basename $1) [-h|--help] [--os]"
        echo "-h, --help         Print help and exit"
        echo "    --os=PATH      Path to VM image used in these tests"
        echo "    --fiobin=PATH  Path to fio binary on host"
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 && exit 0 ;;
			os=*) os_image="${OPTARG#*=}" ;;
			fiobin=*) fio_bin="${OPTARG#*=}" ;;
			*) usage $0 echo "Invalid argument '$OPTARG'" && exit 1 ;;
		esac
		;;
		h) usage $0 && exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'" && exit 1 ;;
	esac
done

source $ROOT_DIR/test/vhost/common/common.sh
trap 'on_error_exit ${FUNCNAME} - ${LINENO}' ERR

source $BASE_DIR/common.sh

if [ ! -x $fio_bin ]; then
	error "Invalid path of fio binary"
fi

function create_bdev_config()
{
	local malloc_name=""
	local virtio_bdevs=""
	local bdevs

	if ! $RPC_PY get_bdevs | jq -r '.[] .name' | grep -qi "Nvme0n1"$; then
		error "Nvme0n1 bdev not found!"
	fi

	cp $BASE_DIR/bdev.conf.in $BASE_DIR/bdev.conf

	$RPC_PY add_vhost_scsi_lun naa.Nvme0n1.0 0 Nvme0n1
	sed -i "s|/tmp/vhost.0|$ROOT_DIR/../vhost/naa.Nvme0n1.0|g" $BASE_DIR/bdev.conf

	malloc_name=$($RPC_PY construct_malloc_bdev 128 512)
	$RPC_PY add_vhost_scsi_lun naa."$malloc_name".1 0 $malloc_name
	sed -i "s|/tmp/vhost.1|$ROOT_DIR/../vhost/naa."$malloc_name".1|g" $BASE_DIR/bdev.conf

	malloc_name=$($RPC_PY construct_malloc_bdev 128 4096)
	$RPC_PY add_vhost_scsi_lun naa."$malloc_name".2 0 $malloc_name
	sed -i "s|/tmp/vhost.2|$ROOT_DIR/../vhost/naa."$malloc_name".2|g" $BASE_DIR/bdev.conf

	bdevs=$(discover_bdevs $ROOT_DIR $BASE_DIR/bdev.conf /var/tmp/spdk2.sock | jq -r '.[] | select(.claimed == false)')
	for b in $(echo $bdevs | jq -r '.name') ; do
		virtio_bdevs+="$b:"
	done

	cp $BASE_DIR/fio_jobs.fio.in $BASE_DIR/fio_jobs.fio
	sed -i 's/\bfilename\b/&='$virtio_bdevs'/' $BASE_DIR/fio_jobs.fio
}

function bdevio_host_test()
{
	timing_enter bdevio_test
	$ROOT_DIR/test/lib/bdev/bdevio/bdevio $BASE_DIR/bdev.conf
	timing_exit bdevio_test
}

function bdevio_and_fio_guest_test()
{
	start_and_prepare_vm

	timing_enter bdevio_guest
	run_guest_bdevio
	timing_exit bdevio_guest

	timing_enter fio_guest
	run_guest_fio --spdk_conf=/root/bdev.conf "${fio_jobs[${test_type[guest]}]}"
	timing_exit fio_guest

	vm_shutdown_all
}

function fio_host_test()
{
	timing_enter fio_host
	run_host_fio --spdk_conf=$BASE_DIR/bdev.conf "${fio_jobs[${test_type[host]}]}"
	timing_exit fio_host
}

function fio_unmap_host_test()
{
	timing_enter fio_host_unmap
	run_host_fio --spdk_conf=$BASE_DIR/bdev.conf "${fio_jobs[${test_type[unmap]}]}"
	timing_exit fio_host_unmap
}

function fio_4G_host_test()
{
	timing_enter fio_4G_rw_verify
	rm -f $BASE_DIR/fio_jobs.fio
	cp $BASE_DIR/fio_jobs.fio.in $BASE_DIR/fio_jobs.fio
	sed -i 's/\bfilename\b/&=VirtioScsi0t0/' $BASE_DIR/fio_jobs.fio
	run_host_fio --spdk_conf=$BASE_DIR/bdev.conf "${fio_jobs[${test_type[4G]}]}"
	timing_exit fio_4G_rw_verify
}

timing_enter blockdev
spdk_vhost_run $BASE_DIR
create_bdev_config

bdevio_host_test
bdevio_and_fio_guest_test
fio_host_test
fio_unmap_host_test
fio_4G_host_test

rm -f $BASE_DIR/fio_jobs.fio
rm -f $BASE_DIR/bdev.conf
spdk_vhost_kill
timing_exit blockdev
