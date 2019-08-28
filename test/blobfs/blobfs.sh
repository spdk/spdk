#!/usr/bin/env bash

SYSTEM=$(uname -s)
if [ $SYSTEM = "FreeBSD" ] ; then
    echo "blobfs.sh cannot run on FreeBSD currently."
    exit 0
fi

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
rpc_server=/var/tmp/spdk-blobfs.sock
rpc_py="$rootdir/scripts/rpc.py -s $rpc_server"
tmp_file=/tmp/blobfs_file
conf_file=/tmp/blobfs.conf
bdevname=BlobfsBdev

source $rootdir/test/common/autotest_common.sh

function on_error_exit() {
	if [ -n "$blobfs_pid" ]; then
		killprocess $blobfs_pid
	fi

	rm -f $tmp_file
	rm -f $conf_file
	print_backtrace
	exit 1
}

function blobfs_start_app {
	$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server -c ${conf_file} &
	blobfs_pid=$!

	echo "Process blobfs pid: $blobfs_pid"
	waitforlisten $blobfs_pid $rpc_server
}

function blobfs_detect_test() {
	# Detect out there is no blobfs on test bdev
	blobfs_start_app
	result=$($rpc_py blobfs_detect ${bdevname})
	if [ "${result}" != "False" ]; then
		false
	fi

	killprocess $blobfs_pid

	# Create blobfs on test bdev
	$rootdir/test/blobfs/mkfs/mkfs ${conf_file} ${bdevname}

	# Detect out there is a blobfs on test bdev
	blobfs_start_app
	result=$($rpc_py blobfs_detect ${bdevname})
	if [ "${result}" != "True" ]; then
		false
	fi

	killprocess $blobfs_pid
}

timing_enter blobfs

trap 'on_error_exit;' ERR

# Create one temp file as test bdev
dd if=/dev/zero of=${tmp_file} bs=4k count=1M
echo "[AIO]" > ${conf_file}
echo "AIO ${tmp_file} ${bdevname} 4096" >> ${conf_file}

blobfs_detect_test

rm -f $tmp_file
report_test_completion "blobfs"

timing_exit blobfs
