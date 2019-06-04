#!/bin/bash -e

cache_in="/var/run/rpc_cache_in"
cache_out="/var/run/rpc_cache_out"
rpc_doorbell="/var/run/rpc_cache_dorbell"

rm -f "$cache_in"
rm -f "$cache_out"
rm -f "$rpc_doorbell"
mkfifo "$cache_in"
mkfifo "$rpc_doorbell"
touch "$cache_out"

# wrap rpc.py into `script` util in order to get rid of the stdout bufferring
# and to flush each line to provided output as soon as it's printed
script --quiet --return --flush "$cache_out" --command "./scripts/rpc.py -f '$cache_in' -d '$rpc_doorbell'" -q &>/dev/null &
rpc_pid=$!

# FIFOs can be opened only once, so manually get some descriptors for them
exec {rpc_input_fd}>$cache_in
exec {doorbell_fd}<$rpc_doorbell

sleep 0.5

function rpc_cache_run() {
	# clear all previous output
	: > $cache_out
	# send the command
	echo "$@" >&$rpc_input_fd
	# wait until it returns, get rc
	read -r -n1 rc <&$doorbell_fd
	# print the output. `script` outputs CRLF line endings, so get rid of those
	col -b < "$cache_out"
	return $rc
}

time {
	rpc_cache_run "$@" get_spdk_version
	rpc_cache_run "$@" get_bdevs
	malloc="$(rpc_cache_run "$@" construct_malloc_bdev 8 512)"
	rpc_cache_run "$@" get_bdevs
	rpc_cache_run "$@" construct_passthru_bdev -b "$malloc" -p Passthru0
	rpc_cache_run "$@" get_bdevs
}

# close the fds
eval "exec $rpc_input_fd>&-"
eval "exec $doorbell_fd>&-"

wait $rpc_pid

rm -f "$cache_in"
rm -f "$cache_out"
rm -f "$rpc_doorbell"
