#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
rpc_py=$rootdir/scripts/rpc.py
pu_start=0
pu_end=3
additional_blocks=16

source $rootdir/test/common/autotest_common.sh

device=$1
uuid=$2

restore_kill() {
	rm -f $testdir/config/ftl.json
	rm -f $testdir/empty
	rm -f $testdir/testblock
	rm -f $testdir/testblock2
	rm -f $testdir/testblock3

	$rpc_py delete_ftl_bdev -b nvme0
	killprocess $svcpid
	rmmod nbd || true
}

trap "restore_kill; exit 1" SIGINT SIGTERM EXIT

# Extract chunk size
chunk_size=$($rootdir/examples/nvme/identify/identify -r "trtype:PCIe traddr:$device" | grep 'Logical blks per chunk' | sed 's/[^0-9]//g')
band_size=$(($chunk_size*($pu_end-$pu_start+1)))

$rootdir/test/app/bdev_svc/bdev_svc & svcpid=$!
# Wait until bdev_svc starts
waitforlisten $svcpid

if [ -n "$uuid" ]; then
	$rpc_py construct_ftl_bdev -b nvme0 -a $device -l $pu_start-$pu_end -u $uuid -r
else
	$rpc_py construct_ftl_bdev -b nvme0 -a $device -l $pu_start-$pu_end -r
fi

# Load the nbd driver
modprobe nbd
$rpc_py start_nbd_disk nvme0 /dev/nbd0
waitfornbd nbd0

$rpc_py save_config > $testdir/config/ftl.json

# Send band worth of data (some data should be written to 2nd band due to metadata overhead and bad chunks)
dd if=/dev/urandom of=/dev/nbd0 bs=4K count=$band_size
sync
# Make sure the second batch of written data is on the second band
dd if=/dev/urandom of=/dev/nbd0 bs=4K count=$additional_blocks seek=$band_size
sync
dd if=/dev/zero of=$testdir/empty bs=4k count=$additional_blocks
dd if=/dev/nbd0 of=$testdir/testblock bs=4k count=1
$rpc_py stop_nbd_disk /dev/nbd0

# Force kill bdev service (dirty shutdown) and start it again
if kill -0 $svcpid; then
	echo "killing process with pid $svcpid"
	kill -9 $svcpid
	rm /var/run/spdk_bdev-1
fi

$rootdir/test/app/bdev_svc/bdev_svc & svcpid=$!
# Wait until bdev_svc starts
waitforlisten $svcpid

#sleep 2
$rpc_py load_config < $testdir/config/ftl.json

# Without persistent cache, some data should be recoverable
dd if=/dev/nbd0 of=$testdir/testblock2 bs=4k count=1
dd if=/dev/nbd0 of=$testdir/testblock3 bs=4k count=$additional_blocks skip=$band_size
# Verify first 4k block was recovered
cmp $testdir/testblock $testdir/testblock2
# Last 4k blocks should be on second band, and return as 0s
cmp $testdir/empty $testdir/testblock3

report_test_completion occsd_dirty_shutdown

trap - SIGINT SIGTERM EXIT
restore_kill
