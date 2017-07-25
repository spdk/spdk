#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="python $rootdir/scripts/rpc.py"

set -e

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter fs_test
timing_enter start_nvmf_tgt
# Start up the NVMf target in another process
$NVMF_APP -c $testdir/../nvmf.conf &
nvmfpid=$!

trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid ${RPC_PORT}
timing_exit start_nvmf_tgt

bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

modprobe -v nvme-rdma

$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" "" -s SPDK00000000000001 -n "$bdevs"

nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

mkdir -p /mnt/device

devs=`lsblk -l -o NAME | grep nvme`

for dev in $devs; do
	timing_enter parted
	parted -s /dev/$dev mklabel msdos  mkpart primary '0%' '100%'
	timing_exit parted
	sleep 1

	for fstype in "ext4" "btrfs" "xfs"; do
		timing_enter $fstype
		if [ $fstype = ext4 ]; then
			force=-F
		else
			force=-f
		fi

		mkfs.${fstype} $force /dev/${dev}p1

		mount /dev/${dev}p1 /mnt/device
		touch /mnt/device/aaa
		sync
		rm /mnt/device/aaa
		sync
		umount /mnt/device
		timing_exit $fstype
	done

	parted -s /dev/$dev rm 1
done

sync
nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true

$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $nvmfpid
timing_exit fs_test
