#!/usr/bin/env bash

set -e

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

function spdk_vhost_run()
{
# FIXME
# run empty vhost
# add malloc bdev and a scsi controller
}

function spdk_vhost_kill()
{
# FIXME
}

function spdk_bdev_io_run()
{
# FIXME
# run bdev example app with the following in it's config
# [Virtio]
#   Dev User /path/to/socket/file
}

function spdk_bdev_io_kill()
{
# FIXME
}

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for doing automated test"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "-h, --help                print help and exit"
	echo "-x                        set -x for script debug"
	echo "    --work-dir=WORK_DIR   Where to find build file. Must exist. [default: $TEST_DIR]"
	exit 0
}

while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 ;;
			work-dir=*) TEST_DIR="${OPTARG#*=}" ;;
			*) usage $0 "Invalid argument '$OPTARG'" ;;
		esac
		;;
	h) usage $0 ;;
	x) set -x
		x="-x" ;;
	*) usage $0 "Invalid argument '$OPTARG'"
	esac
done
shift $(( OPTIND - 1 ))

spdk_vhost_run

rpc="python $SPDK_BUILD_DIR/scripts/rpc.py "
# bdev_io rpc
rpc+="-s 127.0.0.1 "

spdk_bdev_io_run

bdev="bdev_io app bdev"
$rpc bdev_open $bdev

for i in {1..1000}; do
	$rpc bdev_writev $((($i % 4)+1)) $((($i % 25)+1)) a
	$rpc bdev_readv $((($i % 4)+1)) $((($i % 25)+1)) a
done

$rpc bdev_close $bdev

spdk_bdev_io_kill

spdk_vhost_kill
