#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"

x=""
source $TEST_DIR/scripts/autotest_common.sh
RPC_PORT=5260

function usage()
{
    [[ ! -z $2 ]] && ( echo "$2"; echo ""; )
    echo "Shortcut script for doing automated test"
    echo "Usage: $(basename $1) [OPTIONS]"
    echo
    echo "-h, --help                print help and exit"
    exit 0
}

function vhost_start()
{
    $TEST_DIR/examples/bdev/io/bdev_io -c $BASE_DIR/vhost.conf.in &
    vhost_pid=$!
    echo $vhost_pid > $BASE_DIR/vhost.pid
    waitforlisten $vhost_pid $RPC_PORT
}

function  vhost_kill()
{
    vhost_pid="$(cat $BASE_DIR/vhost.pid)"
    killprocess $vhost_pid
}

rpc_py="$TEST_DIR/scripts/rpc.py "
for total_size in 64 128 32; do
    for block_size in 512 4096; do
        vhost_start
        $BASE_DIR/lvol_test.py $rpc_py $total_size $block_size
        vhost_kill
    done
done







