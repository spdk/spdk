#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

bdf=$(get_first_nvme_bdf)
ctrlr_base="/dev/spdk/nvme"

$SPDK_BIN_DIR/spdk_tgt -m 0x3 &
spdk_tgt_pid=$!
trap 'kill -9 ${spdk_tgt_pid}; exit 1' SIGINT SIGTERM EXIT

waitforlisten $spdk_tgt_pid

$rpc_py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a ${bdf}
$rpc_py bdev_nvme_cuse_register -n Nvme0

sleep 5

if [ ! -c "${ctrlr_base}0" ]; then
	exit 1
fi

$rpc_py bdev_get_bdevs
$rpc_py bdev_nvme_get_controllers

$rpc_py bdev_nvme_cuse_unregister -n Nvme0
sleep 1
if [ -c "${ctrlr_base}0" ]; then
	exit 1
fi

# Verify removing non-existent cuse device
$rpc_py bdev_nvme_cuse_unregister -n Nvme0 && false

$rpc_py bdev_nvme_cuse_register -n Nvme0
sleep 1

if [ ! -c "${ctrlr_base}0" ]; then
	exit 1
fi

# Verify adding same nvme controller twice fails
$rpc_py bdev_nvme_cuse_register -n Nvme0 && false
sleep 1

if [ -c "${ctrlr_base}1" ]; then
	exit 1
fi

$rpc_py bdev_nvme_detach_controller Nvme0

trap - SIGINT SIGTERM EXIT
killprocess $spdk_tgt_pid
