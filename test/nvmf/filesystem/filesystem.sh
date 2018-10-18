#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

set -e

# pass the parameter 'iso' to this script when running it in isolation to trigger rdma device initialization.
# e.g. sudo ./filesystem.sh iso
nvmftestinit $1

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter fs_test

for incapsule in 0 4096; do
	# Start up the NVMf target in another process
	$NVMF_APP -m 0xF &
	nvmfpid=$!

	trap "process_shm --id $NVMF_APP_SHM_ID; killprocess $nvmfpid; nvmftestfini $1; exit 1" SIGINT SIGTERM EXIT

	waitforlisten $nvmfpid
	$rpc_py nvmf_create_transport -t RDMA -u 8192 -p 4 -c $incapsule

	bdevs="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
	bdevs+=" $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

	modprobe -v nvme-rdma

	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
	for bdev in $bdevs; do
		$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 $bdev
	done
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t rdma -a $NVMF_FIRST_TARGET_IP -s 4420

	nvme connect -t rdma -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

	waitforblk "nvme0n1"
	waitforblk "nvme0n2"

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
done

nvmftestfini $1
timing_exit fs_test
