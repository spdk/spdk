#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for enabling VMs"
	echo "Usage: $(basename $1) [OPTIONS] VM..."
	echo
	echo "-h, --help                print help and exit"
	echo "    --work-dir=WORK_DIR   Where to find build file. Must exist. [default: ./..]"
	echo "-a                        Run all VMs in WORK_DIR"
	echo "-x                        set -x for script debug"
	exit 0
}
run_all=false
while getopts 'xah-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 ;;
			work-dir=*) TEST_DIR="${OPTARG#*=}" ;;
			*) usage $0 "Invalid argument '$OPTARG'" ;;
		esac
		;;
	h) usage $0 ;;
	a) run_all=true ;;
	x) set -x ;;
	*) usage $0 "Invalid argument '$OPTARG'"
	esac
done

. $BASE_DIR/common.sh

if [[ $EUID -ne 0 ]]; then
	echo "Go away user come back as root"
	exit 1
fi

if $run_all; then
	vm_run -a
else
	shift $((OPTIND-1))
	echo "INFO: running VMs: $@"
	vm_run "$@"
fi
