#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

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
			*) usage $0 echo "Invalid argument '$OPTARG'" ;;
		esac
		;;
	h) usage $0 ;;
	x) set -x ;;
	*) usage $0 "Invalid argument '$optchar'" ;;
	esac
done

if [[ $EUID -ne 0 ]]; then
	echo "Go away user come back as root"
	exit 1
fi

echo "INFO: $0"
echo

. $BASE_DIR/common.sh

#operation will increasse vhost test coverage
echo "INFO: Starting vhost with additional options "
$VHOST_APP -c $BASE_DIR/vhost.conf.in -S $BASE_DIR -e 0x0 -s 2000 -f $BASE_DIR/vhost.pid -d -q -h
sleep 0.5
echo "INFO: Test done"

#expacting vhost to fail if incorrect option is given
echo "INFO: Starting vhost incorrect option"
$VHOST_APP -x || true
sleep 0.5
echo "INFO: Test done"

#for trace flags, vhost should be build with CONFIG_DEBUG=y parameter, otherwise vhost will exit with error
echo "INFO: Starting vhost with trace_flag option"
$VHOST_APP -t vhost_scsi || true
sleep 0.5
echo "INFO: Test done"

spdk_vhost_run
