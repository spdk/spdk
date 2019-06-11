#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py
pu_start=0
pu_end=3
additional_blocks=16

device=$1
uuid=$2

restore_kill() {
	rm -f $testdir/config/ftl.json
	rm -f $testdir/empty
	rm -f $testdir/testblock
	rm -f $testdir/testfile.md5
	rm -f $testdir/testfile2.md5

	$rpc_py delete_ftl_bdev -b nvme0 || true
	killprocess $svcpid || true
	rmmod nbd || true
}

trap "restore_kill; exit 1" SIGINT SIGTERM EXIT

# Extract chunk size
chunk_size=$($rootdir/examples/nvme/identify/identify -r "trtype:PCIe traddr:$device" |
grep 'Logical blks per chunk' | sed 's/[^0-9]//g')

band_size=$(($chunk_size*($pu_end-$pu_start+1)))

$rootdir/app/spdk_tgt/spdk_tgt & svcpid=$!
# Wait until spdk_tgt starts
waitforlisten $svcpid

if [ -n "$uuid" ]; then
	$rpc_py construct_ftl_bdev -b nvme0 -a $device -l $pu_start-$pu_end -u $uuid -o
else
	$rpc_py construct_ftl_bdev -b nvme0 -a $device -l $pu_start-$pu_end -o
fi

# Load the nbd driver
modprobe nbd
$rpc_py start_nbd_disk nvme0 /dev/nbd0
waitfornbd nbd0

$rpc_py save_config > $testdir/config/ftl.json

# Send band worth of data in 2 steps (some data should be written to 2nd band due to metadata overhead)
dd if=/dev/urandom of=/dev/nbd0 bs=4K count=$(($band_size - $chunk_size)) oflag=dsync
offset=$(($band_size - $chunk_size))

dd if=/dev/urandom of=/dev/nbd0 bs=4K count=$chunk_size seek=$offset oflag=dsync
offset=$(($offset + $chunk_size))

# Save md5 data of first batch (which should be fully on a closed band and recoverable)
dd if=/dev/nbd0 bs=4K count=$(($band_size - $chunk_size)) | md5sum > $testdir/testfile.md5

# Make sure the third batch of written data is fully on the second band
dd if=/dev/urandom of=/dev/nbd0 bs=4K count=$additional_blocks seek=$offset oflag=dsync
offset=$(($offset + $additional_blocks))

$rpc_py stop_nbd_disk /dev/nbd0

# Force kill bdev service (dirty shutdown) and start it again
kill -9 $svcpid
rm -f /dev/shm/spdk_tgt_trace.pid$svcpid
# TODO Adapt this after waitforlisten is expanded
sleep 5

$rootdir/app/spdk_tgt/spdk_tgt & svcpid=$!
# Wait until spdk_tgt starts
waitforlisten $svcpid

# Ftl should recover, though with a loss of data (-o config option)
$rpc_py load_config < $testdir/config/ftl.json

# Write extra data after restore
dd if=/dev/urandom of=/dev/nbd0 bs=4K count=$chunk_size seek=$offset oflag=dsync
# Save md5 data
dd if=/dev/nbd0 bs=4K count=$chunk_size skip=$offset | md5sum > $testdir/testfile2.md5

# Make sure all data will be read from disk
echo 3 > /proc/sys/vm/drop_caches

# Without persistent cache, first batch of data should be recoverable
dd if=/dev/nbd0 bs=4K count=$(($band_size - $chunk_size)) | md5sum -c $testdir/testfile.md5

dd if=/dev/nbd0 of=$testdir/testblock bs=4k count=$additional_blocks skip=$band_size
# Last 4k blocks from before restore should be on second band, and return as 0s
dd if=/dev/zero of=$testdir/empty bs=4k count=$additional_blocks
cmp $testdir/empty $testdir/testblock
# Verify data written after restore is still there
dd if=/dev/nbd0 bs=4K count=$chunk_size skip=$(($band_size + $additional_blocks)) | md5sum -c $testdir/testfile2.md5

report_test_completion ftl_dirty_shutdown

trap - SIGINT SIGTERM EXIT
restore_kill
