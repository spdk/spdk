#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

export SPDK_LIB_DIR="$rootdir/build/lib"
export DPDK_LIB_DIR="$rootdir/dpdk/build/lib"
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$SPDK_LIB_DIR:$DPDK_LIB_DIR

function insert_device() {
	ssh root@$ip 'Beetle --SetGpio "$gpio" HIGH'
	waitforblk $name
	DRIVER_OVERRIDE=$driver $rootdir/scripts/setup.sh
}

function remove_device() {
	ssh root@$ip 'Beetle --SetGpio "$gpio" LOW'
}

ip=$1
gpio=$2
driver=$3
declare -i io_time=5
declare -i kernel_hotplug_time=7

timing_enter hotplug_hw_cfg

# Configure microcontroller
ssh root@$ip 'Beetle --SetGpioDirection "$gpio" OUT'

# Get blk dev name connected to interposer
ssh root@$ip 'Beetle --SetGpio "$gpio" HIGH'
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

$SPDK_EXAMPLE_DIR/hotplug -i 0 -t 100 -n 2 -r 2 2>&1 | tee -a log.txt &
example_pid=$!
trap 'killprocess $example_pid; exit 1' SIGINT SIGTERM EXIT

i=0
while ! grep "Starting I/O" log.txt; do
	[ $i -lt 20 ] || break
	i=$((i + 1))
	sleep 1
done

if ! grep "Starting I/O" log.txt; then
	return 1
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
wait $example_pid
timing_exit wait_for_example

trap - SIGINT SIGTERM EXIT

timing_exit hotplug_hw_test
