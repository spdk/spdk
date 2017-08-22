#!/usr/bin/env bash
set -xe
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"

x=""
source $TEST_DIR/scripts/autotest_common.sh
RPC_PORT=5260

###  Function starts bdev_io app
function bdev_io_start()
{
    $TEST_DIR/examples/bdev/io/bdev_io -c $BASE_DIR/bdev_io.conf.in &
    bdev_io_pid=$!
    echo $bdev_io_pid > $BASE_DIR/bdev_io.pid
    waitforlisten $bdev_io_pid $RPC_PORT
}

###  Function stops vhost bdev_io app
function bdev_io_kill()
{
    bdev_io_pid="$(cat $BASE_DIR/bdev_io.pid)"
    ### Kill with SIGKILL param
    kill -KILL $bdev_io_pid
    sleep 5
    rm $BASE_DIR/bdev_io.pid
}

rpc_py="$TEST_DIR/scripts/rpc.py "

for total_size in 64 128 32; do
    for block_size in 512 4096; do
        bdev_io_start
        ### positive tests
        $BASE_DIR/lvol_test.py $rpc_py $total_size $block_size 1
        bdev_io_kill
        bdev_io_start
        ### negative tests
        $BASE_DIR/lvol_test.py $rpc_py $total_size $block_size 2
        bdev_io_kill
        bdev_io_start
        ### sigterm test
        $BASE_DIR/lvol_test.py $rpc_py $total_size $block_size 3
        rm $BASE_DIR/bdev_io.pid
    done
done
