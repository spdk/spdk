#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

rpc_py=$rootdir/scripts/rpc.py

mount_dir=$(mktemp -d)

while getopts ':u:c:f' opt; do
	case $opt in
		u) uuid=$OPTARG ;;
		c) nv_cache=$OPTARG ;;
		f) fast_shutdown=1 ;;
		?) echo "Usage: $0 [-f] [-u UUID] [-c NV_CACHE_PCI_BDF] BASE_PCI_BDF" && exit 1 ;;
	esac
done
shift $((OPTIND - 1))
device=$1
timeout=240

restore_kill() {
	rm -f $testdir/testfile
	rm -f $testdir/testfile.md5
	rm -f $testdir/config/ftl.json

	killprocess $svcpid
	remove_shm
}

trap "restore_kill; exit 1" SIGINT SIGTERM EXIT

"$SPDK_BIN_DIR/spdk_tgt" &
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

if [ "$fast_shutdown" -eq "1" ]; then
	ftl_construct_args+=" --fast-shutdown"
fi

$rpc_py -t $timeout $ftl_construct_args

(
	echo '{"subsystems": ['
	$rpc_py save_subsystem_config -n bdev
	echo ']}'
) > $testdir/config/ftl.json
$rpc_py bdev_ftl_unload -b ftl0
killprocess $svcpid

# Generate random data and calculate checksum
dd if=/dev/urandom of=$testdir/testfile bs=4K count=256K
md5sum $testdir/testfile > $testdir/testfile.md5

# Write and read back the data, verifying checksum
"$SPDK_BIN_DIR/spdk_dd" --if=$testdir/testfile --ob=ftl0 --json=$testdir/config/ftl.json
"$SPDK_BIN_DIR/spdk_dd" --ib=ftl0 --of=$testdir/testfile --json=$testdir/config/ftl.json --count=262144

md5sum -c $testdir/testfile.md5

# Write second time at overlapped sectors, read back and verify checkum
"$SPDK_BIN_DIR/spdk_dd" --if=$testdir/testfile --ob=ftl0 --json=$testdir/config/ftl.json --seek=131072
"$SPDK_BIN_DIR/spdk_dd" --ib=ftl0 --of=$testdir/testfile --json=$testdir/config/ftl.json --skip=131072 --count=262144

md5sum -c $testdir/testfile.md5

trap - SIGINT SIGTERM EXIT
restore_kill
