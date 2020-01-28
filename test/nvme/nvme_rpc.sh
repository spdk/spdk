#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

bdf=$(iter_pci_class_code 01 08 02 | head -1)

$rootdir/app/spdk_tgt/spdk_tgt -m 0x3 &
spdk_tgt_pid=$!
trap 'kill -9 ${spdk_tgt_pid}; exit 1' SIGINT SIGTERM EXIT

waitforlisten $spdk_tgt_pid

$rpc_py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a ${bdf}

# This test should fail
$rpc_py apply_firmware non_existing_file Nvme0n1 || rv=$?
if [ -z "$rv" ]; then
	exit 1
fi

$rpc_py bdev_nvme_detach_controller Nvme0

trap - SIGINT SIGTERM EXIT
killprocess $spdk_tgt_pid
