#!/usr/bin/env bash

set -e
BASE_DIR=$(readlink -f $(dirname $0))
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)

bdevperf_time=10
bdevperf_pattern=("read" "write" "randread" "randwrite" "randrw -M 50" "rw -M 50" "reset" "verify")
script_debug=false
RPC_PY="$ROOT_DIR/scripts/rpc.py"

function usage() {
        [[ ! -z $2 ]] && ( echo "$2"; echo ""; )
        echo "Script for running vhost initiator tests."
        echo "Usage: $(basename $1) [-h|--help]"
        echo "-h, --help         Print help and exit"
        echo "    --test_time    Runtime of each bdevpref test in seconds (default:10)"
        echo "-x                 set -x for script debug"
}

while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 && exit 0 ;;
			test_time=*) bdevperf_time="${OPTARG#*=}" ;;
			*) usage $0 echo "Invalid argument '$OPTARG'" && exit 1 ;;
		esac
		;;
		h) usage $0 && exit 0 ;;
		x) set -x && script_debug=true ;;
		*) usage $0 "Invalid argument '$optchar'" && exit 1 ;;
	esac
done

if [[ $EUID -ne 0 ]]; then
	echo "Go away user come back as root"
	exit 1
fi

function on_error_exit() {
		set +e
		echo "Error on $1 - $2"
		print_backtrace
		vm_shutdown_all
		spdk_vhost_kill
		exit 1
}

trap 'on_error_exit ${FUNCNAME} - ${LINENO}' ERR
source $ROOT_DIR/test/vhost/common/common.sh
. $BASE_DIR/autotest.config

function create_bdev_config()
{
	local malloc_name=""
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
}

function bdevperf_host_test()
{
	for i in "${bdevperf_pattern[@]}"
	do
		$ROOT_DIR/test/lib/bdev/bdevperf/bdevperf -c $BASE_DIR/bdev.conf -q 16 -s 4096 -w $i -t $bdevperf_time
		echo "INFO: Finised io patern: $i"
	done
}

spdk_vhost_run $BASE_DIR
if ! $script_debug; then
	set +x
fi

create_bdev_config
bdevperf_host_test

rm -f $BASE_DIR/bdev.conf
spdk_vhost_kill
