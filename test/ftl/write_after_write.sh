#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

rpc_py=$rootdir/scripts/rpc.py

fio_kill() {
	rm -f $testdir/testfile.md5
	rm -f $testdir/config/ftl.json

	killprocess $svcpid
	rmmod nbd || true
}

device=$1
cache_device=$2
timeout=240
data_size=$((262144 * 5))

if [[ $CONFIG_FIO_PLUGIN != y ]]; then
	echo "FIO not available"
	exit 1
fi

export FTL_BDEV_NAME=ftl0
export FTL_JSON_CONF=$testdir/config/ftl.json

trap "fio_kill; exit 1" SIGINT SIGTERM EXIT

"$SPDK_BIN_DIR/spdk_tgt" &
svcpid=$!
waitforlisten $svcpid

split_bdev=$(create_base_bdev nvme0 $device $((1024 * 101)))
nv_cache=$(create_nv_cache_bdev nvc0 $cache_device $split_bdev)

l2p_percentage=60
l2p_dram_size_mb=$(($(get_bdev_size $split_bdev) * l2p_percentage / 100 / 1024))

$rpc_py -t $timeout bdev_ftl_create -b ftl0 -d $split_bdev -c $nv_cache --l2p_dram_limit $l2p_dram_size_mb

waitforbdev ftl0

(
	echo '{"subsystems": ['
	$rpc_py save_subsystem_config -n bdev
	echo ']}'
) > $FTL_JSON_CONF
$rpc_py bdev_ftl_unload -b ftl0

killprocess $svcpid

fio_bdev $testdir/config/fio/write_after_write.fio

"$SPDK_BIN_DIR/spdk_tgt" -L ftl_init &
svcpid=$!
waitforlisten $svcpid

$rpc_py load_config < $FTL_JSON_CONF
# Load the nbd driver
modprobe nbd
$rpc_py nbd_start_disk ftl0 /dev/nbd0
waitfornbd nbd0

$rpc_py save_config > $testdir/config/ftl.json

# Calculate checksum of the data written
dd if=/dev/nbd0 bs=4K count=$data_size | md5sum > $testdir/testfile.md5
$rpc_py nbd_stop_disk /dev/nbd0

# Force kill bdev service (dirty shutdown) and start it again
kill -9 $svcpid
rm -f /dev/shm/spdk_tgt_trace.pid$svcpid

"$SPDK_BIN_DIR/spdk_tgt" -L ftl_init &
svcpid=$!
waitforlisten $svcpid

$rpc_py load_config < $testdir/config/ftl.json
waitfornbd nbd0

# Verify that the checksum matches and the data is consistent
dd if=/dev/nbd0 bs=4K count=$data_size | md5sum -c $testdir/testfile.md5

trap - SIGINT SIGTERM EXIT
fio_kill
