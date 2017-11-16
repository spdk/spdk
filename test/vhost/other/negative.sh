#!/usr/bin/env bash

. $(readlink -f $(dirname $0))/../common/common.sh || exit 1

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for running vhost app."
	echo "Usage: $(basename $1) [-x] [-h|--help] [--clean-build] [--work-dir=PATH]"
	echo "-h, --help           print help and exit"
	echo "-x                   Set -x for script debug"

	spdk_vhost_usage_common
	exit 0
}

run_in_background=false
for arg in ${VHOST_TEST_ARGS[@]}; do
	case "$arg" in
		-)
		case "$OPTARG" in
			help) usage $0 ;;
			*) usage $0 echo "Invalid argument '$OPTARG'" ;;
		esac
		;;
	h) usage $0 ;;
	x) set -x ;;
	*) usage $0 "Invalid argument '$optchar'" ;;
	esac
done

trap error_exit ERR

VHOST_APP="$SPDK_BUILD_DIR/app/vhost/vhost"

echo "INFO: Testing vhost command line arguments"
# Printing help will force vhost to exit without error
$VHOST_APP -c /path/to/non_existing_file/conf -S $BASE_DIR -e 0x0 -s 1024 -d -q -h

# Testing vhost create pid file option. Vhost will exit with error as invalid config path is given
if $VHOST_APP -c /path/to/non_existing_file/conf -f $VHOST_DIR/vhost.pid; then
	error "vhost started when specifying invalid config file"
	exit 1
fi

# Expecting vhost to fail if an incorrect argument is given
if $VHOST_APP -x -h; then
	error "vhost started with invalid -x command line option"
	exit 1
fi

# Passing trace flags if spdk is build without CONFIG_DEBUG=y option make vhost exit with error
if ! $VHOST_APP -t vhost_scsi -h;  then
	notice "vhost did not started with trace flags enabled but ignoring this as it might not be a debug build"
fi
