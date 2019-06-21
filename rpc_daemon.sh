#!/bin/bash

set +x
set -e

rpc_cmdsock="/tmp/rpc_cmdsock"
rm -f "$rpc_cmdsock"

./scripts/rpc.py --daemon "$rpc_cmdsock" >/dev/null &
rpc_pid=$!
sleep 1

function rpc_rache() {
	set +x
	local cmd="$@"
	local response="$(nc -U "$rpc_cmdsock" <<< "$cmd")"
	echo "${response:1}"
	set -x
	[ "${response:0:1}" = "0" ]
}

time {
	rpc_rache get_spdk_version
	rpc_rache get_bdevs
	malloc=$(rpc_rache construct_malloc_bdev 8 512)
	rpc_rache get_bdevs
	rpc_rache construct_passthru_bdev -b "$malloc" -p Passthru0
	rpc_rache get_bdevs
}

nc -U "$rpc_cmdsock" <<< "exit_daemon"
wait $rpc_pid
rm -f "$rpc_cmdsock"
