#!/usr/bin/env bash
set -xe
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"

x=""
source $TEST_DIR/scripts/autotest_common.sh
RPC_PORT=5260

rpc_py="$TEST_DIR/scripts/rpc.py"

###  Function starts bdev test app
function bdev_start()
{
    sudo modprobe nbd
    $TEST_DIR/test/lib/bdev/nbd/nbd -c $BASE_DIR/bdev.conf.in \
    -b Malloc0 -n /dev/nbd0 &
    bdev_pid=$!
    echo $bdev_pid > $BASE_DIR/bdev.pid
    waitforlisten $bdev_pid $RPC_PORT
    $rpc_py delete_bdev Malloc0
}

###  Function stops vhost bdev test app
function bdev_kill()
{
    bdev_pid="$(cat $BASE_DIR/bdev.pid)"
    ### Kill with SIGKILL param
    kill -KILL $bdev_pid
    sleep 1
    rm $BASE_DIR/bdev.pid
    sudo rmmod nbd
}

for total_size in 64 128 32; do
    for block_size in 512 4096; do
        bdev_start
        ### positive tests
        $BASE_DIR/lvol_test.py $rpc_py $total_size $block_size 1
        bdev_kill
        bdev_start
        ### negative tests
        $BASE_DIR/lvol_test.py $rpc_py $total_size $block_size 2
        bdev_kill
        bdev_start
        ### sigterm test
        $BASE_DIR/lvol_test.py $rpc_py $total_size $block_size 3
        bdev_kill
    done
done
