#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/setup/common.sh
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=128
MALLOC_BLOCK_SIZE=512

nvmftestinit

function nvmf_filesystem_create() {
	fstype=$1
	nvme_name=$2

	make_filesystem ${fstype} /dev/${nvme_name}p1

	mount /dev/${nvme_name}p1 /mnt/device
	touch /mnt/device/aaa
	sync
	rm /mnt/device/aaa
	sync

	i=0
	while ! umount /mnt/device; do
		[ $i -lt 15 ] || break
		i=$((i + 1))
		sleep 1
	done

	# Make sure the target did not crash
	kill -0 $nvmfpid

	# Make sure the device is still present
	lsblk -l -o NAME | grep -q -w "${nvme_name}"

	# Make sure the partition is still present
	lsblk -l -o NAME | grep -q -w "${nvme_name}p1"
}

function nvmf_filesystem_part() {
	in_capsule=$1

	nvmfappstart -m 0xF
	malloc_name=Malloc1

	$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192 -c $in_capsule
	$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b $malloc_name
	$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s $NVMF_SERIAL
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 $malloc_name
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

	malloc_size=$(($(get_bdev_size $malloc_name) * 1024 * 1024))

	$NVME_CONNECT -t $TEST_TRANSPORT -n "nqn.2016-06.io.spdk:cnode1" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

	waitforserial "$NVMF_SERIAL"
	nvme_name=$(lsblk -l -o NAME,SERIAL | grep -oP "([\w]*)(?=\s+${NVMF_SERIAL})")
	nvme_size=$(sec_size_to_bytes $nvme_name)

	mkdir -p /mnt/device
	if ((nvme_size == malloc_size)); then
		parted -s /dev/${nvme_name} mklabel gpt mkpart SPDK_TEST '0%' '100%'
		partprobe
		sleep 1
	else
		echo "ERR: Created dev size is not as expected"
		exit 1
	fi

	if [ $in_capsule -eq 0 ]; then
		run_test "filesystem_ext4" nvmf_filesystem_create "ext4" ${nvme_name}
		run_test "filesystem_btrfs" nvmf_filesystem_create "btrfs" ${nvme_name}
		run_test "filesystem_xfs" nvmf_filesystem_create "xfs" ${nvme_name}
	else
		run_test "filesystem_in_capsule_ext4" nvmf_filesystem_create "ext4" ${nvme_name}
		run_test "filesystem_in_capsule_btrfs" nvmf_filesystem_create "btrfs" ${nvme_name}
		run_test "filesystem_in_capsule_xfs" nvmf_filesystem_create "xfs" ${nvme_name}
	fi

	parted -s /dev/${nvme_name} rm 1

	sync
	nvme disconnect -n "nqn.2016-06.io.spdk:cnode1"
	waitforserial_disconnect "$NVMF_SERIAL"

	$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

	trap - SIGINT SIGTERM EXIT

	killprocess $nvmfpid
	nvmfpid=
}

run_test "nvmf_filesystem_no_in_capsule" nvmf_filesystem_part 0
run_test "nvmf_filesystem_in_capsule" nvmf_filesystem_part 4096

nvmftestfini
