#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "${BASH_SOURCE[0]}"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/ocf/common.sh

config="
$(nvme_cfg)

[Split]
  Split Nvme0n1 7 128
"
echo "$config" > $curdir/config

# Clear nvme device which we will use in test
clear_nvme

$rootdir/app/iscsi_tgt/iscsi_tgt -c $curdir/config &
spdk_pid=$!

waitforlisten $spdk_pid

# Create ocf on persistent storage

$rpc_py bdev_ocf_create ocfWT  wt Nvme0n1p0 Nvme0n1p1
$rpc_py bdev_ocf_create ocfPT  pt Nvme0n1p2 Nvme0n1p3
$rpc_py bdev_ocf_create ocfWB0 wb Nvme0n1p4 Nvme0n1p5
$rpc_py bdev_ocf_create ocfWB1 wb Nvme0n1p4 Nvme0n1p6

# Sorting bdevs because we dont guarantee that they are going to be
# in the same order after shutdown
($rpc_py bdev_ocf_get_bdevs | jq '(.. | arrays) |= sort') > ./ocf_bdevs

trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid

# Check for ocf persistency after restart
$rootdir/app/iscsi_tgt/iscsi_tgt -c $curdir/config &
spdk_pid=$!

trap 'killprocess $spdk_pid; rm -f $curdir/config ocf_bdevs ocf_bdevs_verify; exit 1' SIGINT SIGTERM EXIT

waitforlisten $spdk_pid

# OCF should be loaded now as well

($rpc_py bdev_ocf_get_bdevs | jq '(.. | arrays) |= sort') > ./ocf_bdevs_verify

diff ocf_bdevs ocf_bdevs_verify

trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid
rm -f $curdir/config ocf_bdevs ocf_bdevs_verify

clear_nvme $bdf
