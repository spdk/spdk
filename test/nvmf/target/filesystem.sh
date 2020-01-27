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
	nvme_name=$2

	if [ $fstype = ext4 ]; then
		force=-F
	else
		force=-f
	fi

	local i=0
	while ! mkfs.${fstype} $force /dev/${nvme_name}p1; do
		[ $i -lt 15 ] || break
		i=$((i+1))
		sleep 1
	done

	mount /dev/${nvme_name}p1 /mnt/device
	touch /mnt/device/aaa
	sync
	rm /mnt/device/aaa
	sync

	i=0
	while ! umount /mnt/device; do
		[ $i -lt 15 ] || break
		i=$((i+1))
		sleep 1
	done

	# Make sure the target did not crash
	kill -0 $nvmfpid

	# Make sure the device is still present
	lsblk -l -o NAME | grep -q -w "${nvme_name}"

	# Make sure the partition is still present
	lsblk -l -o NAME | grep -q -w "${nvme_name}p1"
}

function nvmf_filesystem_part {
	incapsule=$1

	nvmfappstart "-m 0xF"

	$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192 -c $incapsule
	$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc1
	$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s $NVMF_SERIAL
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc1
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

	nvme connect -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

	waitforserial "$NVMF_SERIAL"
	nvme_name=$(lsblk -l -o NAME,SERIAL | grep -oP "([\w]*)(?=\s+${NVMF_SERIAL})")

	mkdir -p /mnt/device

	parted -s /dev/${nvme_name} mklabel msdos  mkpart primary '0%' '100%'
	partprobe
	sleep 1

	if [ $incapsule -eq 0 ]; then
		run_test "filesystem_ext4" nvmf_filesystem_create "ext4" ${nvme_name}
		run_test "filesystem_btrfs" nvmf_filesystem_create "btrfs" ${nvme_name}
		run_test "filesystem_xfs" nvmf_filesystem_create "xfs" ${nvme_name}
	else
		run_test "filesystem_incapsule_ext4" nvmf_filesystem_create "ext4" ${nvme_name}
		run_test "filesystem_incapsule_btrfs" nvmf_filesystem_create "btrfs" ${nvme_name}
		run_test "filesystem_incapsule_xfs" nvmf_filesystem_create "xfs" ${nvme_name}
	fi

	parted -s /dev/${nvme_name} rm 1

	sync
	nvme disconnect -n "nqn.2016-06.io.spdk:cnode1" || true

	$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

	trap - SIGINT SIGTERM EXIT

	killprocess $nvmfpid
	nvmfpid=
}

run_test "nvmf_filesystem_no_incapsule" nvmf_filesystem_part 0
run_test "nvmf_filesystem_incapsule" nvmf_filesystem_part 4096

nvmftestfini
