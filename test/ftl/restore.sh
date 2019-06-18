#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

rpc_py=$rootdir/scripts/rpc.py

mount_dir=$(mktemp -d)
uuid=
nv_cache=

while getopts ':u:c:' opt; do
	case $opt in
		u) uuid=$OPTARG ;;
		c) nv_cache=$OPTARG ;;
		?) echo "Usage: $0 [-u UUID] [-c NV_CACHE_PCI_BDF] OCSSD_PCI_BDF" && exit 1 ;;
	esac
done
shift $((OPTIND -1))
device=$1

restore_kill() {
	if mount | grep $mount_dir; then
		umount $mount_dir
	fi
	rm -rf $mount_dir
	rm -f $testdir/testfile.md5
	rm -f $testdir/testfile2.md5
	rm -f $testdir/config/ftl.json

	$rpc_py delete_ftl_bdev -b nvme0
	killprocess $svcpid
	rmmod nbd || true
}

trap "restore_kill; exit 1" SIGINT SIGTERM EXIT

$rootdir/test/app/bdev_svc/bdev_svc & svcpid=$!
# Wait until bdev_svc starts
waitforlisten $svcpid

if [ -n "$nv_cache" ]; then
	nvc_bdev=$(create_nv_cache_bdev nvc0 $device $nv_cache 4)
fi

ftl_construct_args="construct_ftl_bdev -b nvme0 -a $device -l 0-3"

[ -n "$uuid" ]     && ftl_construct_args+=" -u $uuid"
[ -n "$nv_cache" ] && ftl_construct_args+=" -c $nvc_bdev"

$rpc_py $ftl_construct_args

# Load the nbd driver
modprobe nbd
$rpc_py start_nbd_disk nvme0 /dev/nbd0
waitfornbd nbd0

$rpc_py save_config > $testdir/config/ftl.json

# Prepare the disk by creating ext4 fs and putting a file on it
mkfs.ext4 -F /dev/nbd0
mount /dev/nbd0 $mount_dir
dd if=/dev/urandom of=$mount_dir/testfile bs=4K count=256K
sync
mount -o remount /dev/nbd0 $mount_dir
md5sum $mount_dir/testfile > $testdir/testfile.md5

# Kill bdev service and start it again
umount $mount_dir
killprocess $svcpid

$rootdir/test/app/bdev_svc/bdev_svc -L ftl_init & svcpid=$!
# Wait until bdev_svc starts
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

report_test_completion occsd_restore

trap - SIGINT SIGTERM EXIT
restore_kill
