#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source $rootdir/test/common/autotest_common.sh

mount_dir=$(mktemp -d)
device=$1
uuid=$2

restore_kill() {
	if mount | grep $mount_dir; then
		umount $mount_dir
	fi
	rm -rf $mount_dir
	rm -f $testdir/testfile.md5

	killprocess $svcpid
	rmmod nbd || true
}

trap "restore_kill; exit 1" SIGINT SIGTERM EXIT

$testdir/generate_configs.sh -a $device -n nvme0 -l 0-3 \
	$([ -n "$uuid" ] && echo "-m 0 -u $uuid" || echo "-m 1")
$rootdir/test/app/bdev_svc/bdev_svc -c $testdir/config/ocssd.conf &
svcpid=$!

# Load the nbd driver
modprobe nbd
$testdir/start_nbd.py

# Prepare the disk by creating ext4 fs and putting a file on it
mkfs.ext4 -F /dev/nbd0
mount /dev/nbd0 $mount_dir
dd if=/dev/urandom of=$mount_dir/testfile bs=4K count=256K
md5sum $mount_dir/testfile > $testdir/testfile.md5

# Kill bdev service and start it again
umount $mount_dir
killprocess $svcpid

$testdir/generate_configs.sh -a $device -n nvme0 -l 0-3 -m 0 -u $(cat $testdir/.testfile_nvme0.0)
$rootdir/test/app/bdev_svc/bdev_svc -c $testdir/config/ocssd.conf &
svcpid=$!

$testdir/start_nbd.py

mount /dev/nbd0 $mount_dir
md5sum -c $testdir/testfile.md5

report_test_completion occsd_restore

trap - SIGINT SIGTERM EXIT
restore_kill
