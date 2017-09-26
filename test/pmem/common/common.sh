#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"
rpc_py="$TEST_DIR/scripts/rpc.py "
RPC_PORT=5260

source $TEST_DIR/scripts/autotest_common.sh

function nbd_start()
{

    [[ -f $TEST_DIR/test/pmem/nbd.pid ]] && nbd_kill
    modprobe nbd
    # add Malloc0 bdev as it is required to start the nbd app
    $TEST_DIR/test/lib/bdev/nbd/nbd -c $TEST_DIR/test/pmem/nbd.conf.in \
    -b Malloc0 -n /dev/nbd0 &
    nbd_pid=$!
    echo $nbd_pid > $TEST_DIR/test/pmem/nbd.pid
    if [[ ! -f $TEST_DIR/test/pmem/nbd.pid ]]; then
		echo "No nbd pid file found!"
		false
	fi

    waitforlisten $nbd_pid $RPC_PORT
    # delete Malloc0 as it's no longer needed
    $rpc_py delete_bdev Malloc0
}

function nbd_kill()
{
	if [[ ! -f $TEST_DIR/test/pmem/nbd.pid ]]; then
		echo "No nbd pid file found!"
		false
	fi

    if pkill -F $TEST_DIR/test/pmem/nbd.pid; then
        sleep 1
        rm $TEST_DIR/test/pmem/nbd.pid
    fi

    rmmod nbd
}
