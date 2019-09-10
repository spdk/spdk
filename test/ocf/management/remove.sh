#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

rm -f aio*
truncate -s 128M aio0
truncate -s 128M aio1

echo "
[AIO]
  AIO ./aio0 aio0 512
  AIO ./aio1 aio1 512
" > $curdir/config

$rootdir/app/iscsi_tgt/iscsi_tgt -c $curdir/config &
spdk_pid=$!

waitforlisten $spdk_pid

# Create ocf on persistent storage

$rpc_py construct_ocf_bdev ocfWT  wt aio0 aio1

# Remove ocf it should prevent before automatic load on app restart

$rpc_py delete_ocf_bdev ocfWT

($rpc_py get_ocf_bdevs | jq '(.. | arrays) |= sort') > ./ocf_bdevs

trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid

# Check for ocf persistency after restart
$rootdir/app/iscsi_tgt/iscsi_tgt -c $curdir/config &
spdk_pid=$!

trap 'killprocess $spdk_pid; rm -f aio* $curdir/config ocf_bdevs ocf_bdevs_verify; exit 1' SIGINT SIGTERM EXIT

waitforlisten $spdk_pid

# OCF should be loaded now as well

($rpc_py get_ocf_bdevs | jq '(.. | arrays) |= sort') > ./ocf_bdevs_verify

diff ocf_bdevs ocf_bdevs_verify

trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid
rm -f aio* $curdir/config ocf_bdevs ocf_bdevs_verify
