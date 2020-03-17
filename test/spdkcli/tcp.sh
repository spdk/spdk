#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/spdkcli/common.sh

function err_cleanup() {
	if [ -n "$socat_pid" ]; then
		killprocess $socat_pid || true
	fi
	killprocess $spdk_tgt_pid
}

function waitfortcplisten() {
	# $1 = process pid
	if [ -z "$1" ]; then
		exit 1
	fi

	local ipaddr=$2
	local port=$3

	echo "Waiting for process to start up and listen on TCP/IP Socket $ipaddr:$port..."
	# turn off trace for this loop
	xtrace_disable
	local ret=0
	local i
	for (( i = 40; i != 0; i-- )); do
		# if the process is no longer running, then exit the script
		#  since it means the application crashed
		if ! kill -s 0 $1; then
			echo "ERROR: process (pid: $1) is no longer running"
			ret=1
			break
		fi

		if $rootdir/scripts/rpc.py -t 1 -s "$ipaddr" -p $port rpc_get_methods &>/dev/null; then
			break
		fi

		sleep 0.5
	done

	xtrace_restore
	if (( i == 0 )); then
		echo "ERROR: timeout while waiting for process (pid: $1) to start listening on '$ipaddr:$port'"
		ret=1
	fi
	return $ret
}

IP_ADDRESS="127.0.0.1"
PORT="9998"

trap 'err_cleanup; exit 1' SIGINT SIGTERM EXIT

timing_enter run_spdk_tgt_tcp
$rootdir/app/spdk_tgt/spdk_tgt -m 0x3 -p 0 -s 2048 &
spdk_tgt_pid=$!

waitforlisten $spdk_tgt_pid

# socat will terminate automatically after the connection is closed
socat TCP-LISTEN:$PORT UNIX-CONNECT:$DEFAULT_RPC_ADDR &
socat_pid=$!

# This will issue a rpc request to the spdk target thus validating tcp
waitfortcplisten $spdk_tgt_pid $IP_ADDRESS $PORT

timing_exit run_spdk_tgt_tcp

trap - SIGINT SIGTERM EXIT
killprocess $spdk_tgt_pid
