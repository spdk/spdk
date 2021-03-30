#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh

function scheduler_create_thread() {
	$rpc --plugin scheduler_plugin scheduler_thread_create -n active_pinned -m 0x1 -a 100
	$rpc --plugin scheduler_plugin scheduler_thread_create -n active_pinned -m 0x2 -a 100
	$rpc --plugin scheduler_plugin scheduler_thread_create -n active_pinned -m 0x4 -a 100
	$rpc --plugin scheduler_plugin scheduler_thread_create -n active_pinned -m 0x8 -a 100
	$rpc --plugin scheduler_plugin scheduler_thread_create -n idle_pinned -m 0x1 -a 0
	$rpc --plugin scheduler_plugin scheduler_thread_create -n idle_pinned -m 0x2 -a 0
	$rpc --plugin scheduler_plugin scheduler_thread_create -n idle_pinned -m 0x4 -a 0
	$rpc --plugin scheduler_plugin scheduler_thread_create -n idle_pinned -m 0x8 -a 0

	$rpc --plugin scheduler_plugin scheduler_thread_create -n one_third_active -a 30
	thread_id=$($rpc --plugin scheduler_plugin scheduler_thread_create -n half_active -a 0)
	$rpc --plugin scheduler_plugin scheduler_thread_set_active $thread_id 50

	thread_id=$($rpc --plugin scheduler_plugin scheduler_thread_create -n deleted -a 100)
	$rpc --plugin scheduler_plugin scheduler_thread_delete $thread_id
}

rpc=rpc_cmd

$testdir/scheduler -m 0xF -p 0x2 --wait-for-rpc &
scheduler_pid=$!
trap 'killprocess $scheduler_pid; exit 1' SIGINT SIGTERM EXIT
waitforlisten $scheduler_pid

$rpc framework_set_scheduler dynamic
$rpc framework_start_init

# basic integrity test
run_test "scheduler_create_thread" scheduler_create_thread

trap - SIGINT SIGTERM EXIT
killprocess $scheduler_pid
