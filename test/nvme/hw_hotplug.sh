#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

function beetle_ssh() {
	ssh_opts="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
	if [[ -n $BEETLE_SSH_KEY ]]; then
		ssh_opts+=" -i $(readlink -f $BEETLE_SSH_KEY)"
	fi

	#shellcheck disable=SC2029
	ssh $ssh_opts root@$ip "$@"
}

function insert_device() {
	beetle_ssh 'Beetle --SetGpio "$gpio" HIGH'
	waitforblk $name
	DRIVER_OVERRIDE=$driver $rootdir/scripts/setup.sh
}

function remove_device() {
	beetle_ssh 'Beetle --SetGpio "$gpio" LOW'
}

function restore_device() {
	beetle_ssh 'Beetle --SetGpio "$gpio" HIGH'
}

ip=$1
gpio=$2
driver=$3

declare -i io_time=5
declare -i kernel_hotplug_time=7

timing_enter hotplug_hw_cfg

# Configure microcontroller
beetle_ssh 'Beetle --SetGpioDirection "$gpio" OUT'

# Get blk dev name connected to interposer
beetle_ssh 'Beetle --SetGpio "$gpio" HIGH'
sleep $kernel_hotplug_time
$rootdir/scripts/setup.sh reset
blk_list1=$(lsblk -d --output NAME | grep "^nvme")
remove_device
sleep $kernel_hotplug_time
blk_list2=$(lsblk -d --output NAME | grep "^nvme") || true
name=${blk_list1#"$blk_list2"}

insert_device

timing_exit hotplug_hw_cfg

timing_enter hotplug_hw_test

mode=""
if [ "$driver" = "uio_pci_generic" ]; then
	mode="-m pa"
fi

exec {log}> >(tee -a "$testdir/log.txt")
exec >&$log 2>&1

$SPDK_EXAMPLE_DIR/hotplug -i 0 -t 100 -n 2 -r 2 $mode &
hotplug_pid=$!

trap 'killprocess $hotplug_pid; restore_device; exit 1' SIGINT SIGTERM EXIT

i=0
while ! grep "Starting I/O" $testdir/log.txt; do
	[ $i -lt 20 ] || break
	i=$((i + 1))
	sleep 1
done

if ! grep "Starting I/O" $testdir/log.txt; then
	exit 1
fi

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

timing_exit hotplug_hw_test
