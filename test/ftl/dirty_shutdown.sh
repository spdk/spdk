#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

rpc_py=$rootdir/scripts/rpc.py

while getopts ':u:c:' opt; do
	case $opt in
		u) uuid=$OPTARG ;;
		c) nv_cache=$OPTARG ;;
		?) echo "Usage: $0 [-u UUID] [-c NV_CACHE_PCI_BDF] OCSSD_PCI_BDF" && exit 1 ;;
	esac
done
shift $((OPTIND - 1))

device=$1
use_ocssd=$2
timeout=240

zone_size=$[262144]
write_unit_size=16
data_size=$(( $zone_size * 2 ))
chunk_size=$zone_size

restore_kill() {
	rm -f $testdir/config/ftl.json
	rm -f $testdir/testfile.md5
	rm -f $testdir/testfile2.md5

	killprocess $svcpid || true
	rmmod nbd || true
	remove_shm
}

trap "restore_kill; exit 1" SIGINT SIGTERM EXIT

"$SPDK_BIN_DIR/spdk_tgt" --json <(gen_ftl_nvme_conf) &
svcpid=$!
# Wait until spdk_tgt starts
waitforlisten $svcpid

$rpc_py bdev_nvme_attach_controller -b nvme0 -a $device -t pcie
split_bdev=$($rootdir/scripts/rpc.py bdev_split_create nvme0n1 -s $((1024*101))  1)

if [ -n "$nv_cache" ]; then
	nvc_bdev=$(create_nv_cache_bdev nvc0 $nv_cache $split_bdev)
fi

l2p_dram_size_mb=$(( $(get_bdev_size $split_bdev)/1024*10/100 ))
ftl_construct_args="bdev_ftl_create -b ftl0 -d $split_bdev --l2p_dram_limit $l2p_dram_size_mb"

[ -n "$uuid" ] && ftl_construct_args+=" -u $uuid"
[ -n "$nv_cache" ] && ftl_construct_args+=" -c $nvc_bdev"

$rpc_py -t $timeout $ftl_construct_args

# Load the nbd driver
modprobe nbd
$rpc_py nbd_start_disk ftl0 /dev/nbd0
waitfornbd nbd0

$rpc_py save_config > $testdir/config/ftl.json

dd if=/dev/urandom of=/dev/nbd0 bs=4K count=$data_size oflag=dsync
# Calculate checksum of the data written
dd if=/dev/nbd0 bs=4K count=$data_size | md5sum > $testdir/testfile.md5
$rpc_py nbd_stop_disk /dev/nbd0

# Force kill bdev service (dirty shutdown) and start it again
kill -9 $svcpid
rm -f /dev/shm/spdk_tgt_trace.pid$svcpid

"$SPDK_BIN_DIR/spdk_tgt" --json <(gen_ftl_nvme_conf) -L ftl_init &
svcpid=$!
waitforlisten $svcpid

$rpc_py load_config < $testdir/config/ftl.json
waitfornbd nbd0

# Write extra data after restore
dd if=/dev/urandom of=/dev/nbd0 bs=4K count=$chunk_size seek=$data_size oflag=dsync
# Save md5 data
dd if=/dev/nbd0 bs=4K count=$chunk_size skip=$data_size | md5sum > $testdir/testfile2.md5

# Make sure all data will be read from disk
echo 3 > /proc/sys/vm/drop_caches

# Verify that the checksum matches and the data is consistent
dd if=/dev/nbd0 bs=4K count=$data_size | md5sum -c $testdir/testfile.md5
dd if=/dev/nbd0 bs=4K count=$chunk_size skip=$data_size | md5sum -c $testdir/testfile2.md5

trap - SIGINT SIGTERM EXIT
restore_kill
