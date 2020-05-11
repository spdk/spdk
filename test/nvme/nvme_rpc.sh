#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

bdf=$(get_first_nvme_bdf)

$SPDK_BIN_DIR/spdk_tgt -m 0x3 &
spdk_tgt_pid=$!
trap 'kill -9 ${spdk_tgt_pid}; exit 1' SIGINT SIGTERM EXIT

waitforlisten $spdk_tgt_pid

$rpc_py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a ${bdf}

# 1) Test bdev_nvme_apply_firmware RPC
#    NOTE: We don't want to do real firmware update on CI

# Make sure that used firmware file doesn't exist
if [ -f non_existing_file ]; then
	exit 1
fi

# a) Try to apply firmware from non existing file
$rpc_py bdev_nvme_apply_firmware non_existing_file Nvme0n1 || rv=$?
if [ -z "$rv" ]; then
	exit 1
fi

$rpc_py bdev_nvme_detach_controller Nvme0

trap - SIGINT SIGTERM EXIT
killprocess $spdk_tgt_pid
