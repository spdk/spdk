#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for running vhost app."
	echo "Usage: $(basename $1) [-x] [-h|--help] [--clean-build] [--work-dir=PATH]"
	echo "-h, --help           print help and exit"
	echo "-x                   Set -x for script debug"
	echo "    --work-dir=PATH  Where to find source/project. [default=$TEST_DIR]"

	exit 0
}

run_in_background=false
while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 ;;
			work-dir=*) TEST_DIR="${OPTARG#*=}" ;;
			conf-dir=*) CONF_DIR="${OPTARG#*=}" ;;
			*) usage $0 echo "Invalid argument '$OPTARG'" ;;
		esac
		;;
	h) usage $0 ;;
	x) set -x ;;
	*) usage $0 "Invalid argument '$optchar'" ;;
	esac
done


. $COMMON_DIR/common.sh

trap error_exit ERR

VHOST_APP="$SPDK_BUILD_DIR/app/vhost/vhost"

notice "Testing vhost command line arguments"
# Printing help will force vhost to exit without error
$VHOST_APP -c /path/to/non_existing_file/conf -S $BASE_DIR -e 0x0 -s 1024 -d -q -h

# Testing vhost create pid file option. Vhost will exit with error as invalid config path is given
if $VHOST_APP -c /path/to/non_existing_file/conf -f $SPDK_VHOST_SCSI_TEST_DIR/vhost.pid; then
	fail "vhost started when specifying invalid config file"
fi

# Expecting vhost to fail if an incorrect argument is given
if $VHOST_APP -x -h; then
	fail "vhost started with invalid -x command line option"
fi

# Passing trace flags if spdk is build without CONFIG_DEBUG=y option make vhost exit with error
if ! $VHOST_APP -t vhost_scsi -h;  then
	warning "vhost did not started with trace flags enabled but ignoring this as it might not be a debug build"
fi
