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

IP_ADDRESS="127.0.0.1"
PORT="9998"

trap 'err_cleanup; exit 1' SIGINT SIGTERM EXIT

timing_enter run_spdk_tgt_tcp
$SPDK_BIN_DIR/spdk_tgt -m 0x3 -p 0 -s 2048 &
spdk_tgt_pid=$!

waitforlisten $spdk_tgt_pid

# socat will terminate automatically after the connection is closed
socat TCP-LISTEN:$PORT UNIX-CONNECT:$DEFAULT_RPC_ADDR &
socat_pid=$!

$rootdir/scripts/rpc.py -r 100 -t 2 -s $IP_ADDRESS -p $PORT rpc_get_methods

timing_exit run_spdk_tgt_tcp

trap - SIGINT SIGTERM EXIT
killprocess $spdk_tgt_pid
