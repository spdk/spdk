#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit

function nvmf_filesystem_create {
	fstype=$1

	if [ $fstype = ext4 ]; then
		force=-F
	else
		force=-f
	fi

	mkfs.${fstype} $force /dev/nvme0n1p1

	mount /dev/nvme0n1p1 /mnt/device
	touch /mnt/device/aaa
	sync
	rm /mnt/device/aaa
	sync
	umount /mnt/device
}

function nvmf_filesystem_part {
	incapsule=$1

	nvmfappstart "-m 0xF"

	$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192 -c $incapsule
	$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc1
	$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc1
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

	nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

	# TODO: fix this to wait for the proper NVMe device.
	# if we are hosting the local filesystem on an NVMe drive, this test will fail
	# because it relies on the no other NVMe drives being present in the system.
	waitforblk "nvme0n1"

	mkdir -p /mnt/device

	parted -s /dev/nvme0n1 mklabel msdos  mkpart primary '0%' '100%'
	partprobe
	sleep 1

	if [ $incapsule -eq 0 ]; then
		run_test "case" "filesystem_ext4" nvmf_filesystem_create "ext4"
		run_test "case" "filesystem_btrfs" nvmf_filesystem_create "btrfs"
		run_test "case" "filesystem_xfs" nvmf_filesystem_create "xfs"
	else
		run_test "case" "filesystem_incapsule_ext4" nvmf_filesystem_create "ext4"
		run_test "case" "filesystem_incapsule_btrfs" nvmf_filesystem_create "btrfs"
		run_test "case" "filesystem_incapsule_xfs" nvmf_filesystem_create "xfs"
	fi

	parted -s /dev/nvme0n1 rm 1

	sync
	nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true

	$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

	trap - SIGINT SIGTERM EXIT

	killprocess $nvmfpid
}

run_test "suite" "nvmf_filesystem_no_incapsule" nvmf_filesystem_part 0
run_test "suite" "nvmf_filesystem_incapsule" nvmf_filesystem_part 4096

nvmftestfini
