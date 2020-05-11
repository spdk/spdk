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

restore_kill() {
	rm -f $testdir/config/ftl.json
	rm -f $testdir/testfile.md5
	rm -f $testdir/testfile2.md5

	killprocess $svcpid || true
	rmmod nbd || true
}

trap "restore_kill; exit 1" SIGINT SIGTERM EXIT

chunk_size=$(get_chunk_size $device)
num_group=$(get_num_group $device)
num_pu=$(get_num_pu $device)
pu_count=$((num_group * num_pu))

# Write one band worth of data + one extra chunk
data_size=$((chunk_size * (pu_count + 1)))

"$SPDK_BIN_DIR/spdk_tgt" --json <(gen_ftl_nvme_conf) &
svcpid=$!
waitforlisten $svcpid

if [ -n "$nv_cache" ]; then
	nvc_bdev=$(create_nv_cache_bdev nvc0 $device $nv_cache $pu_count)
fi

$rpc_py bdev_nvme_attach_controller -b nvme0 -a $device -t pcie
$rpc_py bdev_ocssd_create -c nvme0 -b nvme0n1 -n 1
ftl_construct_args="bdev_ftl_create -b ftl0 -d nvme0n1 -o"

[ -n "$nvc_bdev" ] && ftl_construct_args+=" -c $nvc_bdev"
[ -n "$uuid" ] && ftl_construct_args+=" -u $uuid"

$rpc_py $ftl_construct_args

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
