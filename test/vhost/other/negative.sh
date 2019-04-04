#!/usr/bin/env bash

NEGATIVE_BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $NEGATIVE_BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $NEGATIVE_BASE_DIR/../../../../ && pwd)"

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
$VHOST_APP -c /path/to/non_existing_file/conf -S $NEGATIVE_BASE_DIR -e 0x0 -s 1024 -d -h --silence-noticelog

# Testing vhost create pid file option. Vhost will exit with error as invalid config path is given
if $VHOST_APP -c /path/to/non_existing_file/conf -f $SPDK_VHOST_SCSI_TEST_DIR/vhost.pid; then
	fail "vhost started when specifying invalid config file"
fi

# Testing vhost start with invalid config. Vhost will exit with error as bdev module init failed
if $VHOST_APP -c $NEGATIVE_BASE_DIR/invalid.config; then
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

if [[ $RUN_NIGHTLY -eq 1 ]]; then
	# Run with valid config and try some negative rpc calls
	notice "==============="
	notice ""
	notice "running SPDK"
	notice ""
	spdk_vhost_run --json-path=$NEGATIVE_BASE_DIR
	notice ""

	rpc_py="$SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

	# General commands
	notice "Trying to remove nonexistent controller"
	if $rpc_py remove_vhost_controller unk0 > /dev/null; then
		error "Removing nonexistent controller succeeded, but it shouldn't"
	fi

	# SCSI
	notice "Trying to create scsi controller with incorrect cpumask"
	if $rpc_py construct_vhost_scsi_controller vhost.invalid.cpumask --cpumask 0x2; then
		error "Creating scsi controller with incorrect cpumask succeeded, but it shouldn't"
	fi

	notice "Trying to remove device from nonexistent scsi controller"
	if $rpc_py remove_vhost_scsi_target vhost.nonexistent.name 0; then
		error "Removing device from nonexistent scsi controller succeeded, but it shouldn't"
	fi

	notice "Trying to add device to nonexistent scsi controller"
	if $rpc_py add_vhost_scsi_lun vhost.nonexistent.name 0 Malloc0; then
		error "Adding device to nonexistent scsi controller succeeded, but it shouldn't"
	fi

	notice "Trying to create scsi controller with incorrect name"
	if $rpc_py construct_vhost_scsi_controller .; then
		error "Creating scsi controller with incorrect name succeeded, but it shouldn't"
	fi

	notice "Creating controller naa.0"
	$rpc_py construct_vhost_scsi_controller naa.0

	notice "Adding initial device (0) to naa.0"
	$rpc_py add_vhost_scsi_lun naa.0 0 Malloc0

	notice "Trying to remove nonexistent device on existing controller"
	if $rpc_py remove_vhost_scsi_target naa.0 1 > /dev/null; then
		error "Removing nonexistent device (1) from controller naa.0 succeeded, but it shouldn't"
	fi

	notice "Trying to remove existing device from a controller"
	$rpc_py remove_vhost_scsi_target naa.0 0

	notice "Trying to remove a just-deleted device from a controller again"
	if $rpc_py remove_vhost_scsi_target naa.0 0 > /dev/null; then
		error "Removing device 0 from controller naa.0 succeeded, but it shouldn't"
	fi

	notice "Re-adding device 0 to naa.0"
	$rpc_py add_vhost_scsi_lun naa.0 0 Malloc0

	# BLK
	notice "Trying to create block controller with incorrect cpumask"
	if $rpc_py construct_vhost_blk_controller vhost.invalid.cpumask  Malloc0 --cpumask 0x2; then
		error "Creating block controller with incorrect cpumask succeeded, but it shouldn't"
	fi

	notice "Trying to remove nonexistent block controller"
	if $rpc_py remove_vhost_controller vhost.nonexistent.name; then
		error "Removing nonexistent block controller succeeded, but it shouldn't"
	fi

	notice "Trying to create block controller with incorrect name"
	if $rpc_py construct_vhost_blk_controller . Malloc0; then
		error "Creating block controller with incorrect name succeeded, but it shouldn't"
	fi

	notice "Testing done -> shutting down"
	notice "killing vhost app"
	spdk_vhost_kill

	notice "EXIT DONE"
	notice "==============="
fi
