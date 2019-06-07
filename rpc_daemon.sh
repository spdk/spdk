#!/bin/bash

set -x
set -e

rm -f /tmp/socketname

./scripts/rpc.py --daemon &>/dev/null &
rpc_pid=$!
sleep 1

function rpc_rache() {
	set +x
	local cmd="$@"
	local response="$(nc -U /tmp/socketname <<< "$cmd")"
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

echo "$rpc_pid"
rm /tmp/socketname
kill $rpc_pid
wait $rpc_pid
