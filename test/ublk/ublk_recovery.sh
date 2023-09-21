#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.
#
testdir=$(readlink -f "$(dirname $0)")
rootdir=$(readlink -f "$testdir/../..")
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/lvol/common.sh"

modprobe ublk_drv

function cleanup() {
	killprocess $spdk_pid
}

# start `spdk_tgt`
"$SPDK_BIN_DIR/spdk_tgt" -m 0x3 -L ublk &
spdk_pid=$!
trap 'cleanup; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

rpc_cmd ublk_create_target
rpc_cmd bdev_malloc_create -b malloc0 64 4096
rpc_cmd ublk_start_disk malloc0 1 -q 2 -d 128

sleep 1

# run `fio` in other cores
taskset -c 2-3 fio --name=fio_test --filename=/dev/ublkb1 --numjobs=1 --iodepth=128 --ioengine=libaio --rw=randrw --direct=1 --time_based --runtime=60 &
fio_proc=$!

sleep 5

# use `kill -9` so that `spdk_tgt` exit without destroying ublk device
kill -9 $spdk_pid

sleep 5

# restart `spdk_tgt`
"$SPDK_BIN_DIR/spdk_tgt" -m 0x3 -L ublk &
spdk_pid=$!
trap 'cleanup; exit 1' SIGINT SIGTERM EXIT
waitforlisten $spdk_pid

# send recovery command to `ublk_drv`
rpc_cmd ublk_create_target
rpc_cmd bdev_malloc_create -b malloc0 64 4096
rpc_cmd ublk_recover_disk malloc0 1

# wait for `fio` exit
wait $fio_proc

# cleanup and exit
rpc_cmd ublk_stop_disk 1
rpc_cmd ublk_destroy_target

trap - SIGINT SIGTERM EXIT
cleanup
