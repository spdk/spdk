#!/usr/bin/env bash

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
	if mount | grep $mount_dir; then
		umount $mount_dir
	fi
	rm -rf $mount_dir
	rm -f $testdir/testfile.md5
	rm -f $testdir/testfile2.md5
	rm -f $testdir/config/ftl.json

	killprocess $svcpid
	rmmod nbd || true
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

$rpc_py -t $timeout $ftl_construct_args

# Load the nbd driver
modprobe nbd
$rpc_py nbd_start_disk ftl0 /dev/nbd0
waitfornbd nbd0

$rpc_py save_config > $testdir/config/ftl.json

# Prepare the disk by creating ext4 fs and putting a file on it
make_filesystem ext4 /dev/nbd0
mount /dev/nbd0 $mount_dir
dd if=/dev/urandom of=$mount_dir/testfile bs=4K count=256K
sync
mount -o remount /dev/nbd0 $mount_dir
md5sum $mount_dir/testfile > $testdir/testfile.md5
umount $mount_dir

# Kill bdev service and start it again
if [ "$fast_shutdown" -eq "1" ]; then
	$rpc_py bdev_ftl_delete -b ftl0 --fast_shutdown
else
	$rpc_py bdev_ftl_delete -b ftl0
fi

killprocess $svcpid

"$SPDK_BIN_DIR/spdk_tgt" -L ftl_init &
svcpid=$!
# Wait until spdk_tgt starts
waitforlisten $svcpid

$rpc_py load_config < $testdir/config/ftl.json
waitfornbd nbd0

mount /dev/nbd0 $mount_dir

# Write second file, to make sure writer thread has restored properly
dd if=/dev/urandom of=$mount_dir/testfile2 bs=4K count=256K
md5sum $mount_dir/testfile2 > $testdir/testfile2.md5

# Make sure second file will be read from disk
echo 3 > /proc/sys/vm/drop_caches

# Check both files have proper data
md5sum -c $testdir/testfile.md5
md5sum -c $testdir/testfile2.md5

trap - SIGINT SIGTERM EXIT
restore_kill
