#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"
rpc_py="$TEST_DIR/scripts/rpc.py "
RPC_PORT=5260

source $TEST_DIR/scripts/autotest_common.sh

function nbd_start()
{
    modprobe nbd
    $TEST_DIR/test/lib/bdev/nbd/nbd -c $TEST_DIR/test/pmem/nbd.conf.in \
    -b Malloc0 -n /dev/nbd0 &
    nbd_pid=$!
    echo $nbd_pid > $BASE_DIR/nbd.pid
    waitforlisten $nbd_pid $RPC_PORT
}

function nbd_kill()
{
    if pkill -F $BASE_DIR/nbd.pid; then
        sleep 1
        rm $BASE_DIR/nbd.pid
    fi
    rmmod nbd
}