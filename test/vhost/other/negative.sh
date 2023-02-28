#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

function usage() {
	[[ -n $2 ]] && (
		echo "$2"
		echo ""
	)
	echo "Shortcut script for running vhost app."
	echo "Usage: $(basename $1) [-x] [-h|--help] [--clean-build]"
	echo "-h, --help           print help and exit"
	echo "-x                   Set -x for script debug"

	exit 0
}

run_in_background=false
while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
			case "$OPTARG" in
				help) usage $0 ;;
				conf-dir=*) CONF_DIR="${OPTARG#*=}" ;;
				*) usage $0 echo "Invalid argument '$OPTARG'" ;;
			esac
			;;
		h) usage $0 ;;
		x) set -x ;;
		*) usage $0 "Invalid argument '$optchar'" ;;
	esac
done

vhosttestinit

trap error_exit ERR

notice "Testing vhost command line arguments"
# Printing help will force vhost to exit without error
"${VHOST_APP[@]}" -c /path/to/non_existing_file/conf -S $testdir -e 0x0 -s 1024 -d -h --silence-noticelog

# Testing vhost create pid file option. Vhost will exit with error as invalid config path is given
if "${VHOST_APP[@]}" -c /path/to/non_existing_file/conf -f "$VHOST_DIR/vhost/vhost.pid"; then
	fail "vhost started when specifying invalid config file"
fi
rm -f $VHOST_DIR/vhost/vhost.pid

# Expecting vhost to fail if an incorrect argument is given
if "${VHOST_APP[@]}" -x -h; then
	fail "vhost started with invalid -x command line option"
fi

# Passing trace flags if spdk is build without CONFIG_DEBUG=y option make vhost exit with error
if ! "${VHOST_APP[@]}" -t vhost_scsi -h; then
	warning "vhost did not started with trace flags enabled but ignoring this as it might not be a debug build"
fi

# Run with valid config and try some negative rpc calls
notice "==============="
notice ""
notice "running SPDK"
notice ""
vhost_run -n 0 -a "-m 0xf"
notice ""
rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"
$rpc_py bdev_malloc_create -b Malloc0 128 4096
$rpc_py bdev_malloc_create -b Malloc1 128 4096
$rpc_py bdev_malloc_create -b Malloc2 128 4096
$rpc_py bdev_split_create Malloc2 8

# Try to get nonexistent vhost controller
if $rpc_py vhost_get_controllers -n nonexistent; then
	error "vhost returned controller that does not exist"
fi

notice "Set coalescing for nonexistent controller"
if $rpc_py vhost_controller_set_coalescing nonexistent 1 100; then
	error "Set coalescing for nonexistent controller should fail"
fi

# General commands
notice "Trying to remove nonexistent controller"
if $rpc_py vhost_delete_controller unk0 > /dev/null; then
	error "Removing nonexistent controller succeeded, but it shouldn't"
fi

# SCSI
notice "Trying to create scsi controller with incorrect cpumask outside of application cpumask"
if $rpc_py vhost_create_scsi_controller vhost.invalid.cpumask --cpumask 0xf0; then
	error "Creating scsi controller with incorrect cpumask succeeded, but it shouldn't"
fi

notice "Trying to create scsi controller with incorrect cpumask partially outside of application cpumask"
if $rpc_py vhost_create_scsi_controller vhost.invalid.cpumask --cpumask 0xff; then
	error "Creating scsi controller with incorrect cpumask succeeded, but it shouldn't"
fi

notice "Trying to remove device from nonexistent scsi controller"
if $rpc_py vhost_scsi_controller_remove_target vhost.nonexistent.name 0; then
	error "Removing device from nonexistent scsi controller succeeded, but it shouldn't"
fi

notice "Trying to add device to nonexistent scsi controller"
if $rpc_py vhost_scsi_controller_add_target vhost.nonexistent.name 0 Malloc0; then
	error "Adding device to nonexistent scsi controller succeeded, but it shouldn't"
fi

notice "Trying to create scsi controller with incorrect name"
if $rpc_py vhost_create_scsi_controller .; then
	error "Creating scsi controller with incorrect name succeeded, but it shouldn't"
fi

notice "Creating controller naa.0"
$rpc_py vhost_create_scsi_controller naa.0

notice "Pass invalid parameter for vhost_controller_set_coalescing"
if $rpc_py vhost_controller_set_coalescing naa.0 -1 100; then
	error "Set coalescing with invalid parameter should fail"
fi

notice "Trying to add nonexistent device to scsi controller"
if $rpc_py vhost_scsi_controller_add_target naa.0 0 nonexistent_bdev; then
	error "Adding nonexistent device to scsi controller succeeded, but it shouldn't"
fi

notice "Adding device to naa.0 with slot number exceeding max"
if $rpc_py vhost_scsi_controller_add_target naa.0 8 Malloc0; then
	error "Adding device to naa.0 should fail but succeeded"
fi

for i in $(seq 0 7); do
	$rpc_py vhost_scsi_controller_add_target naa.0 -1 Malloc2p$i
done
notice "All slots are occupied. Try to add one more device to naa.0"
if $rpc_py vhost_scsi_controller_add_target naa.0 -1 Malloc0; then
	error "Adding device to naa.0 should fail but succeeded"
fi
for i in $(seq 0 7); do
	$rpc_py vhost_scsi_controller_remove_target naa.0 $i
done

notice "Adding initial device (0) to naa.0"
$rpc_py vhost_scsi_controller_add_target naa.0 0 Malloc0

notice "Adding device to naa.0 with slot number 0"
if $rpc_py vhost_scsi_controller_add_target naa.0 0 Malloc1; then
	error "Adding device to naa.0 occupied slot should fail but succeeded"
fi

notice "Trying to remove nonexistent device on existing controller"
if $rpc_py vhost_scsi_controller_remove_target naa.0 1 > /dev/null; then
	error "Removing nonexistent device (1) from controller naa.0 succeeded, but it shouldn't"
fi

notice "Trying to remove existing device from a controller"
$rpc_py vhost_scsi_controller_remove_target naa.0 0

notice "Trying to remove a just-deleted device from a controller again"
if $rpc_py vhost_scsi_controller_remove_target naa.0 0 > /dev/null; then
	error "Removing device 0 from controller naa.0 succeeded, but it shouldn't"
fi

notice "Trying to remove scsi target with invalid slot number"
if $rpc_py vhost_scsi_controller_remove_target naa.0 8 > /dev/null; then
	error "Removing device 8 from controller naa.0 succeeded, but it shouldn't"
fi

# BLK
notice "Trying to create block controller with incorrect cpumask outside of application cpumask"
if $rpc_py vhost_create_blk_controller vhost.invalid.cpumask Malloc0 --cpumask 0xf0; then
	error "Creating block controller with incorrect cpumask succeeded, but it shouldn't"
fi

notice "Trying to create block controller with incorrect cpumask partially outside of application cpumask"
if $rpc_py vhost_create_blk_controller vhost.invalid.cpumask Malloc0 --cpumask 0xff; then
	error "Creating block controller with incorrect cpumask succeeded, but it shouldn't"
fi

notice "Trying to remove nonexistent block controller"
if $rpc_py vhost_delete_controller vhost.nonexistent.name; then
	error "Removing nonexistent block controller succeeded, but it shouldn't"
fi

notice "Trying to create block controller with incorrect name"
if $rpc_py vhost_create_blk_controller . Malloc0; then
	error "Creating block controller with incorrect name succeeded, but it shouldn't"
fi

notice "Trying to create block controller with nonexistent bdev"
if $rpc_py vhost_create_blk_controller blk_ctrl Malloc3; then
	error "Creating block controller with nonexistent bdev succeeded, but shouldn't"
fi

notice "Trying to create block controller with claimed bdev"
$rpc_py bdev_lvol_create_lvstore Malloc0 lvs
if $rpc_py vhost_create_blk_controller blk_ctrl Malloc0; then
	error "Creating block controller with claimed bdev succeeded, but shouldn't"
fi
$rpc_py bdev_lvol_delete_lvstore -l lvs

notice "Testing done -> shutting down"
notice "killing vhost app"
vhost_kill 0

notice "EXIT DONE"
notice "==============="

vhosttestfini
