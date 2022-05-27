#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

rpc_py=$rootdir/scripts/rpc.py

fio_kill() {
	rm -f $testdir/testfile.md5
	rm -f $testdir/config/ftl.json
	rm -f $testdir/random_pattern

	killprocess $svcpid
}

device=$1
cache_device=$2
timeout=240
data_size=$(( 262144 ))


if [[ $CONFIG_FIO_PLUGIN != y ]]; then
	echo "FIO not available"
	exit 1
fi

export FTL_BDEV_NAME=ftl0
export FTL_JSON_CONF=$testdir/config/ftl.json

trap "fio_kill; exit 1" SIGINT SIGTERM EXIT

"$SPDK_BIN_DIR/spdk_tgt" -m 1f --json <(gen_ftl_nvme_conf) &
svcpid=$!
waitforlisten $svcpid

$rpc_py bdev_nvme_attach_controller -b nvme0 -a $device -t pcie
split_bdev=$($rootdir/scripts/rpc.py bdev_split_create nvme0n1 -s $((1024*101))  1)
nv_cache=$(create_nv_cache_bdev nvc0 $cache_device $split_bdev)

l2p_percentage=60
l2p_dram_size_mb=$(( $(get_bdev_size $split_bdev)/1024*$l2p_percentage/100 ))

$rpc_py -t $timeout bdev_ftl_create -b ftl0 -d $split_bdev -c $nv_cache --core_mask 7 --l2p_dram_limit $l2p_dram_size_mb --overprovisioning 10

waitforbdev ftl0

(
    echo '{"subsystems": ['
    $rpc_py save_subsystem_config -n bdev
    echo ']}'
) > $FTL_JSON_CONF

bdev_info=$($rpc_py bdev_get_bdevs -b ftl0)
nb=$(jq ".[] .num_blocks" <<< "$bdev_info")

killprocess $svcpid

dd if=/dev/urandom bs=4K count=$data_size > $testdir/random_pattern

"$SPDK_BIN_DIR/spdk_tgt" --json <(gen_ftl_nvme_conf) -L ftl_init &
svcpid=$!
waitforlisten $svcpid

$rpc_py load_config < $FTL_JSON_CONF
# Load the nbd driver
modprobe nbd
$rpc_py nbd_start_disk ftl0 /dev/nbd0
waitfornbd nbd0

$rpc_py save_config > $testdir/config/ftl_nbd.json

# Calculate checksum of the data written
dd if=$testdir/random_pattern of=/dev/nbd0 bs=4K count=$data_size oflag=direct

sync && echo 3 > /proc/sys/vm/drop_caches

# Unmap first 4MiB
$rpc_py bdev_ftl_unmap  -b ftl0 --lba 0 --num_blocks 1024
$rpc_py bdev_ftl_unmap  -b ftl0 --lba $(($nb - 1024)) --num_blocks 1024

sync && echo 3 > /proc/sys/vm/drop_caches

# Calculate checksum of the data written
file=$testdir/data
dd if=/dev/nbd0 bs=4K count=$data_size of=$file iflag=direct
md5sum $file > $testdir/testfile.md5

sync && echo 3 > /proc/sys/vm/drop_caches

dd if=/dev/nbd0 bs=4K count=$data_size of=$file iflag=direct
md5sum -c $testdir/testfile.md5
sync && echo 3 > /proc/sys/vm/drop_caches

$rpc_py nbd_stop_disk /dev/nbd0
killprocess $svcpid

"$SPDK_BIN_DIR/spdk_tgt" --json <(gen_ftl_nvme_conf) -L ftl_init &
svcpid=$!
waitforlisten $svcpid

$rpc_py load_config < $testdir/config/ftl_nbd.json
waitfornbd nbd0

# Verify that the checksum matches and the data is consistent
sync && echo 3 > /proc/sys/vm/drop_caches
dd if=/dev/nbd0 bs=4K count=$data_size of=$file iflag=direct
md5sum -c $testdir/testfile.md5

$rpc_py nbd_stop_disk /dev/nbd0
# Kill bdev service and start it again
if [ "$fast_shdn" -eq "1" ]; then
    $rpc_py delete_ftl_bdev -b ftl0 --fast_shutdown
fi

killprocess $svcpid

"$SPDK_BIN_DIR/spdk_tgt" --json <(gen_ftl_nvme_conf) -L ftl_init &
svcpid=$!
waitforlisten $svcpid

$rpc_py load_config < $FTL_JSON_CONF
# Load the nbd driver
modprobe nbd
$rpc_py nbd_start_disk ftl0 /dev/nbd0
waitfornbd nbd0

dd if=$testdir/random_pattern of=/dev/nbd0 bs=4K count=$data_size oflag=direct
sync && echo 3 > /proc/sys/vm/drop_caches

# Unmap first 4MiB
$rpc_py bdev_ftl_unmap  -b ftl0 --lba 0 --num_blocks 1024
$rpc_py bdev_ftl_unmap  -b ftl0 --lba $(($nb - 1024)) --num_blocks 1024

$rpc_py nbd_stop_disk /dev/nbd0
# Kill bdev service and start it again
if [ "$fast_shdn" -eq "1" ]; then
    $rpc_py delete_ftl_bdev -b ftl0 --fast_shutdown
fi

killprocess $svcpid

"$SPDK_BIN_DIR/spdk_tgt" --json <(gen_ftl_nvme_conf) -L ftl_init &
svcpid=$!
waitforlisten $svcpid

$rpc_py load_config < $testdir/config/ftl_nbd.json
waitfornbd nbd0

# Verify that the checksum matches and the data is consistent
dd if=/dev/nbd0 bs=4K count=$data_size of=$file iflag=direct
md5sum -c $testdir/testfile.md5

trap - SIGINT SIGTERM EXIT
fio_kill
