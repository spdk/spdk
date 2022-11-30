#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

rpc_py=$rootdir/scripts/rpc.py
spdk_dd="$SPDK_BIN_DIR/spdk_dd"

while getopts ':u:c:' opt; do
	case $opt in
		u) uuid=$OPTARG ;;
		c) nv_cache=$OPTARG ;;
		?) echo "Usage: $0 [-u UUID] [-c NV_CACHE_PCI_BDF] BASE_PCI_BDF" && exit 1 ;;
	esac
done
shift $((OPTIND - 1))

device=$1
timeout=240

block_size=4096
chunk_size=262144
data_size=$chunk_size

restore_kill() {
	rm -f $testdir/config/ftl.json
	rm -f $testdir/testfile
	rm -f $testdir/testfile2
	rm -f $testdir/testfile.md5
	rm -f $testdir/testfile2.md5

	killprocess $svcpid || true
	rmmod nbd || true
	remove_shm
}

trap "restore_kill; exit 1" SIGINT SIGTERM EXIT

"$SPDK_BIN_DIR/spdk_tgt" -m 0x1 &
svcpid=$!
# Wait until spdk_tgt starts
waitforlisten $svcpid

split_bdev=$(create_base_bdev nvme0 $device $((1024 * 101)))

if [ -n "$nv_cache" ]; then
	nvc_bdev=$(create_nv_cache_bdev nvc0 $nv_cache $split_bdev)
fi

l2p_dram_size_mb=$(($(get_bdev_size $split_bdev) * 10 / 100 / 1024))
ftl_construct_args="bdev_ftl_create -b ftl0 -d $split_bdev --l2p_dram_limit $l2p_dram_size_mb"

[ -n "$uuid" ] && ftl_construct_args+=" -u $uuid"
[ -n "$nv_cache" ] && ftl_construct_args+=" -c $nvc_bdev"

$rpc_py -t $timeout $ftl_construct_args

(
	echo '{"subsystems": ['
	$rpc_py save_subsystem_config -n bdev
	echo ']}'
) > $testdir/config/ftl.json

# Load the nbd driver
modprobe nbd
$rpc_py nbd_start_disk ftl0 /dev/nbd0
waitfornbd nbd0

# Write and calculate checksum of the data written
$spdk_dd -m 0x2 -r /var/tmp/spdk_dd.sock --if=/dev/urandom --of=$testdir/testfile --bs=$block_size --count=$data_size
md5sum $testdir/testfile > $testdir/testfile.md5
$spdk_dd -m 0x2 -r /var/tmp/spdk_dd.sock --if=$testdir/testfile --of=/dev/nbd0 --bs=$block_size --count=$data_size --oflag=direct
sync /dev/nbd0
$rpc_py nbd_stop_disk /dev/nbd0
$rpc_py bdev_ftl_unload -b ftl0

# Force kill bdev service (dirty shutdown) and start it again
kill -9 $svcpid
rm -f /dev/shm/spdk_tgt_trace.pid$svcpid

# Write extra data after restore
$spdk_dd --if=/dev/urandom --of=$testdir/testfile2 --bs=$block_size --count=$chunk_size
$spdk_dd --if=$testdir/testfile2 --ob=ftl0 --count=$chunk_size --seek=$data_size --json=$testdir/config/ftl.json
# Save md5 data
md5sum $testdir/testfile2 > $testdir/testfile2.md5

# Verify that the checksum matches and the data is consistent
$spdk_dd --ib=ftl0 --of=$testdir/testfile --count=$data_size --json=$testdir/config/ftl.json
md5sum -c $testdir/testfile.md5
$spdk_dd --ib=ftl0 --of=$testdir/testfile2 --count=$chunk_size --skip=$data_size --json=$testdir/config/ftl.json
md5sum -c $testdir/testfile2.md5

trap - SIGINT SIGTERM EXIT
restore_kill
