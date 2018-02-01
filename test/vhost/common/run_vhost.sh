#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

vhost_num=""

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for running vhost app."
	echo "Usage: $(basename $1) [-x] [-h|--help] [--clean-build] [--work-dir=PATH]"
	echo "-h, --help           print help and exit"
	echo "-x                   Set -x for script debug"
	echo "    --gdb            Run app under gdb"
	echo "    --gdbserver      Run app under gdb-server"
	echo "    --work-dir=PATH  Where to find source/project. [default=$TEST_DIR]"
	echo "    --conf-dir=PATH  Path to directory with configuration for vhost"
	echo "    --vhost-num=NUM  Optional: vhost instance NUM to start. Default: 0"

	exit 0
}

run_in_background=false
while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 ;;
			gdb) VHOST_GDB="gdb --args" ;;
			gdbserver) VHOST_GDB="gdbserver 127.0.0.1:12345"
				;;
			work-dir=*) TEST_DIR="${OPTARG#*=}" ;;
			conf-dir=*) CONF_DIR="${OPTARG#*=}" ;;
			vhost-num=*) vhost_num="${OPTARG}" ;;
			*) usage $0 echo "Invalid argument '$OPTARG'" ;;
		esac
		;;
	h) usage $0 ;;
	x) set -x ;;
	*) usage $0 "Invalid argument '$optchar'" ;;
	esac
done

if [[ $EUID -ne 0 ]]; then
	fail "Go away user come back as root"
fi

notice "$0"
notice ""

. $COMMON_DIR/common.sh

# Starting vhost with valid options
spdk_vhost_run $vhost_num --conf-path=$CONF_DIR
