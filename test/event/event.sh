#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/bdev/nbd_common.sh

function app_repeat_test() {
	local rpc_server=/var/tmp/spdk-nbd.sock
	local nbd_list=("/dev/nbd0" "/dev/nbd1")
	local bdev_list=("Malloc0" "Malloc1")
	local repeat_times=4

	modprobe nbd
	$rootdir/test/event/app_repeat/app_repeat -r $rpc_server -m 0x3 -t $repeat_times &
	repeat_pid=$!
	trap 'killprocess $repeat_pid; exit 1' SIGINT SIGTERM EXIT
	echo "Process app_repeat pid: $repeat_pid"

	for i in {0..2}; do
		echo "spdk_app_start Round $i"
		waitforlisten $repeat_pid $rpc_server

		$rootdir/scripts/rpc.py -s $rpc_server bdev_malloc_create 64 4096
		$rootdir/scripts/rpc.py -s $rpc_server bdev_malloc_create 64 4096

		nbd_rpc_data_verify $rpc_server "${bdev_list[*]}" "${nbd_list[*]}"
		# This SIGTERM is sent to the app_repeat test app - it doesn't actually
		# terminate the app, it just causes it go through another
		# spdk_app_stop/spdk_app_start cycle
		./scripts/rpc.py -s $rpc_server spdk_kill_instance SIGTERM
	done

	waitforlisten $repeat_pid $rpc_server
	killprocess $repeat_pid
	trap - SIGINT SIGTERM EXIT

	return 0
}

run_test "event_perf" $testdir/event_perf/event_perf -m 0xF -t 1
run_test "event_reactor" $testdir/reactor/reactor -t 1
run_test "event_reactor_perf" $testdir/reactor_perf/reactor_perf -t 1

if [ $(uname -s) = Linux ]; then
	run_test "event_scheduler" $testdir/scheduler/scheduler.sh
	if modprobe -n nbd; then
		run_test "app_repeat" app_repeat_test
	fi
fi
