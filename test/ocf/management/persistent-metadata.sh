#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

rm -f aio*
fallocate -l 128M aio0
fallocate -l 128M aio1
fallocate -l 128M aio2
fallocate -l 128M aio3
fallocate -l 128M aio4
fallocate -l 128M aio5
fallocate -l 128M aio6

echo "
[AIO]
  AIO ./aio0 aio0 512
  AIO ./aio1 aio1 512
  AIO ./aio2 aio2 512
  AIO ./aio3 aio3 512
  AIO ./aio4 aio4 512
  AIO ./aio5 aio5 512
  AIO ./aio6 aio6 512
" > $curdir/config

$rootdir/app/iscsi_tgt/iscsi_tgt -c $curdir/config &
spdk_pid=$!

waitforlisten $spdk_pid

# Create ocf on persistent storage

$rpc_py construct_ocf_bdev ocfWT  wt aio0 aio1
$rpc_py construct_ocf_bdev ocfPT  pt aio2 aio3
$rpc_py construct_ocf_bdev ocfWB0 wb aio4 aio5
$rpc_py construct_ocf_bdev ocfWB1 wb aio4 aio6

# Sorting bdevs because we dont guarantee that they are going to be
# in the same order after shutdown
($rpc_py get_ocf_bdevs | jq '(.. | arrays) |= sort') > ./ocf_bdevs

trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid

# Check for ocf persistency after restart
$rootdir/app/iscsi_tgt/iscsi_tgt -c $curdir/config &
spdk_pid=$!

trap "killprocess $spdk_pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $spdk_pid

# OCF should be loaded now as well

($rpc_py get_ocf_bdevs | jq '(.. | arrays) |= sort') > ./ocf_bdevs_verify

diff ocf_bdevs ocf_bdevs_verify

trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid
rm -f aio* $curdir/config ocf_bdevs ocf_bdevs_verify
