#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for shutting down VMs"
	echo "Usage: $(basename $1) [OPTIONS] [VMs]"
	echo
	echo "-h, --help                print help and exit"
	echo "    --work-dir=WORK_DIR   Where to find build file. Must exist. [default: ./..]"
	echo "-a                        kill/shutdown all running VMs"
	echo "-k                        kill instead of shutdown"
	exit 0
}
optspec='akh-:'
do_kill=false
all=false

while getopts "$optspec" optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help)       usage $0 ;;
			work-dir=*) TEST_DIR="${OPTARG#*=}" ;;
			*)           usage $0 "Invalid argument '$OPTARG'" ;;
		esac
		;;
	h) usage $0 ;;
	k) do_kill=true ;;
	a) all=true ;;
	*) usage $0 "Invalid argument '$OPTARG'"
	esac
done

. $BASE_DIR/common.sh

if $do_kill && [[ $EUID -ne 0 ]]; then
	echo "Go away user come back as root"
	exit 1
fi

if $all; then
	if do_kill; then
		echo 'INFO: killing all VMs'
		vm_kill_all
	else
		echo 'INFO: shutting down all VMs'
		vm_shutdown_all
	fi
else
	shift $((OPTIND-1))

	if do_kill; then
		echo 'INFO: killing VMs: $@'
		for vm in $@; do
			vm_kill $vm
		done
	else
		echo 'INFO: shutting down all VMs'
		vm_shutdown_all
	fi
fi
