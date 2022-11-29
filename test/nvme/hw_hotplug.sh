#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#
shopt -s nullglob

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

export PYTHONPATH="$rootdir/examples/nvme/hotplug/"
rpc_py=$rootdir/scripts/rpc.py

function beetle_ssh() {
	ssh_opts="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
	if [[ -n $BEETLE_SSH_KEY ]]; then
		ssh_opts+=" -i $(readlink -f $BEETLE_SSH_KEY)"
	fi

	#shellcheck disable=SC2029
	ssh $ssh_opts root@$ip "$@"
}

function insert_device() {
	beetle_ssh 'for gpio in {0..10}; do Beetle --SetGpio "$gpio" HIGH; done'
	for name in "${names[@]}"; do
		waitforblk $name
	done
	DRIVER_OVERRIDE=$driver $rootdir/scripts/setup.sh
}

function remove_device() {
	beetle_ssh 'for gpio in {0..10}; do Beetle --SetGpio "$gpio" LOW; done'
}

function restore_device() {
	beetle_ssh 'for gpio in {0..10}; do Beetle --SetGpio "$gpio" HIGH; done'
	# Bind all devices to kernel
	"$rootdir/scripts/setup.sh" reset
}

ip=$1
driver=$2

declare -i io_time=5
declare -i kernel_hotplug_time=7

timing_enter hotplug_hw_cfg

# Configure microcontroller
beetle_ssh 'for gpio in {0..10}; do Beetle --SetGpioDirection "$gpio" OUT; done'

# Get blk dev names connected to interposer
restore_device
sleep $kernel_hotplug_time
blk_list1=$(lsblk -d --output NAME | grep "^nvme")
remove_device
sleep $kernel_hotplug_time
blk_list2=$(lsblk -d --output NAME | grep "^nvme") || true

names=(${blk_list1#"$blk_list2"})

nvme_count="${#names[@]}"
echo nvme_count

# Move devices back to userspace
insert_device

timing_exit hotplug_hw_cfg

timing_enter hotplug_hw_test

mode=""
if [ "$driver" = "uio_pci_generic" ]; then
	mode="-m pa"
fi

"$SPDK_EXAMPLE_DIR/hotplug" -i 0 -t 100 -n $((2 * nvme_count)) -r $((2 * nvme_count)) \
	$mode --wait-for-rpc &
hotplug_pid=$!

trap 'killprocess $hotplug_pid; restore_device; exit 1' SIGINT SIGTERM EXIT

waitforlisten $hotplug_pid
$rpc_py --plugin hotplug_plugin perform_tests

# Add and remove NVMe with delays between to give some time for IO to proceed
remove_device
sleep $io_time
insert_device
sleep $io_time
remove_device
sleep $io_time
insert_device
sleep $io_time

timing_enter wait_for_example

if ! wait $hotplug_pid; then
	echo "Hotplug example returned error!"
	exit 1
fi

timing_exit wait_for_example

trap - SIGINT SIGTERM EXIT

restore_device

timing_exit hotplug_hw_test
