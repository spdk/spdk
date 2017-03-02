#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for connecting to or executing command on selected VM"
	echo "Usage: $(basename $1) [OPTIONS] VM_NUMBER"
	echo
	echo "-h, --help                print help and exit"
	echo "    --work-dir=WORK_DIR   Where to find build file. Must exist. [default: $TEST_DIR]"
	echo "-w                        Don't wait for vm to boot"
	echo "-x                        set -x for script debug"
	exit 0
}

boot_wait=true
while getopts 'xwh-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help)	usage $0 ;;
			work-dir=*) TEST_DIR="${OPTARG#*=}" ;;
			*) usage $0 "Invalid argument '$OPTARG'" ;;
		esac ;;
	h) usage $0 ;;
	w) boot_wait=false ;;
	x) set -x ;;
	*) usage $0 "Invalid argument '$OPTARG'" ;;
	esac
done

. $BASE_DIR/common.sh

shift $((OPTIND-1))
vm_num="$1"
shift


if ! vm_num_is_valid $vm_num; then
	usage $0 "Invalid VM num $vm_num"
	exit 1
fi

if $boot_wait; then
	while ! vm_os_booted $vm_num; do
		if ! vm_is_running $vm_num; then
			echo "ERROR: VM$vm_num is not running"
				exit 1
		fi
		echo "INFO: waiting for VM$vm_num to boot"
		sleep 1
	done
fi

vm_ssh $vm_num "$@"
