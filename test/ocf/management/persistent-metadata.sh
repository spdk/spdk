#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

rm -f aio0 aio1 aio2 aio3
truncate -s 128M aio0
truncate -s 128M aio1
truncate -s 128M aio2
truncate -s 128M aio3

echo "
[AIO]
  AIO ./aio0 aio0 512
  AIO ./aio1 aio1 512
  AIO ./aio2 aio2 512
  AIO ./aio3 aio3 512
" > $curdir/config

$rootdir/app/iscsi_tgt/iscsi_tgt -c $curdir/config &
spdk_pid=$!

waitforlisten $spdk_pid

# Create ocf on persistent storage

$rpc_py construct_ocf_bdev ocf0 wt aio0 aio1
$rpc_py construct_ocf_bdev ocf1 wt aio0 aio2
$rpc_py construct_ocf_bdev ocf2 wt aio0 aio3

$rpc_py get_ocf_bdevs > ./ocf_bdevs

trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid

# Check for ocf persistency after restart
$rootdir/app/iscsi_tgt/iscsi_tgt -c $curdir/config &
spdk_pid=$!

trap "killprocess $spdk_pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $spdk_pid

# OCF should be loaded now as well

$rpc_py get_ocf_bdevs > ./ocf_bdevs_verify

diff ocf_bdevs ocf_bdevs_verify

trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid
rm aio0 aio1 aio2 aio3 ocf_bdevs ocf_bdevs_verify $curdir/config
