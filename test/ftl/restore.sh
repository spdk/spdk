#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

mount_dir=$(mktemp -d)

device=$1
config=$SPDK_TEST_STORAGE/ftl.json

restore_kill() {
	if mount | grep $mount_dir; then
		umount $mount_dir
	fi
	rm -rf $mount_dir
	rm -f "$SPDK_TEST_STORAGE/testfile.md5"
	rm -f "$SPDK_TEST_STORAGE/testfile2.md5"
	rm -f "$config"

	killprocess $svcpid
	rmmod nbd || true
}

trap "restore_kill; exit 1" SIGINT SIGTERM EXIT

"$SPDK_BIN_DIR/spdk_tgt" --json <(gen_ftl_nvme_conf) &
svcpid=$!
# Wait until spdk_tgt starts
waitforlisten $svcpid

$rpc_py bdev_nvme_attach_controller -b nvme0 -a $device -t pcie
bdev_create_zone nvme0n1
ftl_construct_args="bdev_ftl_create -b ftl0 -d $ZONE_DEV"

$rpc_py $ftl_construct_args

# Load the nbd driver
modprobe nbd
$rpc_py nbd_start_disk ftl0 /dev/nbd0
waitfornbd nbd0

$rpc_py save_config > "$config"

# Prepare the disk by creating ext4 fs and putting a file on it
make_filesystem ext4 /dev/nbd0
mount /dev/nbd0 $mount_dir
dd if=/dev/urandom of=$mount_dir/testfile bs=4K count=4k
sync
mount -o remount /dev/nbd0 $mount_dir
md5sum $mount_dir/testfile > "$SPDK_TEST_STORAGE/testfile.md5"

# Kill bdev service and start it again
umount $mount_dir
killprocess $svcpid

"$SPDK_BIN_DIR/spdk_tgt" --json <(gen_ftl_nvme_conf) -L ftl_init &
svcpid=$!
# Wait until spdk_tgt starts
waitforlisten $svcpid

$rpc_py load_config < "$config"
waitfornbd nbd0

mount /dev/nbd0 $mount_dir

# Write second file, to make sure writer thread has restored properly
dd if=/dev/urandom of=$mount_dir/testfile2 bs=4K count=4k
md5sum $mount_dir/testfile2 > "$SPDK_TEST_STORAGE/testfile2.md5"

# Make sure second file will be read from disk
echo 3 > /proc/sys/vm/drop_caches

# Check both files have proper data
md5sum -c "$SPDK_TEST_STORAGE/testfile.md5"
md5sum -c "$SPDK_TEST_STORAGE/testfile2.md5"

trap - SIGINT SIGTERM EXIT
restore_kill
