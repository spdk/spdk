#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

function insert_device() {
	ssh root@$ip 'Beetle --SetGpio "$gpio" HIGH'
	sleep 10
	$rootdir/scripts/setup.sh
}

function remove_device() {
	ssh root@$ip 'Beetle --SetGpio "$gpio" LOW'
}

ip=$1
gpio=$2

timing_enter hotplug_hw

# Configure microcontroller
ssh root@$ip 'Beetle --SetGpioDirection "$gpio" OUT'

insert_device

timing_enter hotplug_hw_test

$rootdir/examples/nvme/hotplug/hotplug -i 0 -t 100 -n 2 -r 2 &
example_pid=$!
sleep 20

remove_device
sleep 7
insert_device
sleep 7
remove_device
sleep 7
insert_device
sleep 7

timing_enter wait_for_example
wait $example_pid
timing_exit wait_for_example

trap - SIGINT SIGTERM EXIT

report_test_completion "nvme_hotplug_hw"
timing_exit hotplug_hw_test

timing_exit hotplug_hw
