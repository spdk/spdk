#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

timing_enter fs_test

nvmftestinit

for incapsule in 0 4096; do
	nvmfappstart "-m 0xF"

	$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192 -c $incapsule

	$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc1
	$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc1
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

	nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

	waitforblk "nvme0n1"

	mkdir -p /mnt/device

	devs=$(lsblk -l -o NAME | grep nvme)

	for dev in $devs; do
		timing_enter parted
		parted -s /dev/$dev mklabel msdos  mkpart primary '0%' '100%'
		partprobe
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

nvmftestfini
timing_exit fs_test
