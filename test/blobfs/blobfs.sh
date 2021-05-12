#!/usr/bin/env bash

SYSTEM=$(uname -s)
if [ $SYSTEM = "FreeBSD" ]; then
	echo "blobfs.sh cannot run on FreeBSD currently."
	exit 0
fi

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

rpc_server=/var/tmp/spdk-blobfs.sock
rpc_py="$rootdir/scripts/rpc.py -s $rpc_server"
tmp_file=$SPDK_TEST_STORAGE/blobfs_file
conf_file=$testdir/config
bdevname=BlobfsBdev
mount_dir=/tmp/spdk_tmp_mount
test_cache_size=512

function cleanup() {
	if [[ -n $blobfs_pid && -e /proc/$blobfs_pid ]]; then
		killprocess $blobfs_pid
	fi

	rm -rf $mount_dir
	rm -f $tmp_file
	rm -f $conf_file
}

function blobfs_start_app() {
	$rootdir/test/app/bdev_svc/bdev_svc -r $rpc_server --json ${conf_file} &
	blobfs_pid=$!

	echo "Process blobfs pid: $blobfs_pid"
	waitforlisten $blobfs_pid $rpc_server

	result=$($rpc_py blobfs_set_cache_size ${test_cache_size})
	if [ "${result}" != "True" ]; then
		false
	fi
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

function blobfs_create_test() {
	blobfs_start_app

	# Create blobfs on test bdev
	$rpc_py blobfs_create ${bdevname}

	# Detect out there is a blobfs on test bdev
	result=$($rpc_py blobfs_detect ${bdevname})
	if [ "${result}" != "True" ]; then
		false
	fi

	killprocess $blobfs_pid
}

function blobfs_fuse_test() {
	if [ ! -d /usr/include/fuse3 ] && [ ! -d /usr/local/include/fuse3 ]; then
		echo "libfuse3 is not installed which is required to this test."
		return 0
	fi

	# mount blobfs on test dir
	$rootdir/test/blobfs/fuse/fuse ${conf_file} ${bdevname} $mount_dir &
	blobfs_pid=$!
	echo "Process blobfs pid: $blobfs_pid"

	# Currently blobfs fuse APP doesn't support specific path of RPC sock.
	# So directly use default sock path.
	waitforlisten $blobfs_pid /var/tmp/spdk.sock

	sleep 1

	# check mount status
	mount | grep "$mount_dir"

	# create a rand file in mount dir
	dd if=/dev/urandom of=${mount_dir}/rand_file bs=4k count=32

	umount ${mount_dir}
	sleep 1
	killprocess $blobfs_pid

	# Verify there is no file in mount dir now
	if [ -f ${mount_dir}/rand_file ]; then
		false
	fi

	# use blobfs mount RPC
	blobfs_start_app
	$rpc_py blobfs_mount ${bdevname} $mount_dir

	# read and delete the rand file
	md5sum ${mount_dir}/rand_file
	rm ${mount_dir}/rand_file

	umount ${mount_dir}
	sleep 1
	killprocess $blobfs_pid
}

trap 'cleanup' EXIT

# Create one temp file as test bdev
dd if=/dev/zero of=${tmp_file} bs=4k count=1M

jq . <<- JSON > ${conf_file}
	{
	  "subsystems": [
	    {
	      "subsystem": "bdev",
	      "config": [
	        {
	          "method": "bdev_aio_create",
	          "params": {
	            "name": "${bdevname}",
	            "block_size": 512,
	            "filename": "${tmp_file}"
	          }
	        },
	        {
	          "method": "bdev_wait_for_examine"
		}
	      ]
	    }
	  ]
	}
JSON

blobfs_detect_test

# Clear blobfs on temp file
dd if=/dev/zero of=${tmp_file} bs=4k count=1M

blobfs_create_test

# Create dir for FUSE mount
mkdir -p $mount_dir
blobfs_fuse_test
