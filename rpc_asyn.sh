#!/bin/bash -xe

rootdir=$(readlink -f $(dirname $0))

shopt -s expand_aliases

function on_exit() {
	trap - EXIT

	if [[ -n "$n_pid" ]]; then
		kill $n_pid;
	fi


}

trap "on_exit" EXIT ERR

: ${socket=$rootdir/../sandbox/vhost0/vhost.sock}

alias rpc="$rootdir/scripts/rpc.py -s $socket"

rpc -v get_notification_types
sleep 1

# TODO: max-count test
rpc get_notifications -t 1000 &
n_pid=$!

sleep 0.5
rpc -t 3600 construct_malloc_bdev -b Malloc2 32 512
rpc -t 3600 delete_malloc_bdev Malloc2
rpc -t 3600 construct_malloc_bdev -b Malloc3 32 512
rpc -t 3600 construct_malloc_bdev -b Malloc4 32 512


#sleep 1
rpc -t 3600 delete_malloc_bdev Malloc3
rpc -t 3600 delete_malloc_bdev Malloc4

sleep 1
#rpc -t 3600 construct_malloc_bdev -b Malloc2 32 512
#rpc -t 3600 delete_malloc_bdev Malloc2

wait $n_pid

trap - EXIT ERR
echo "DONE"
