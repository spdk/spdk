#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

function filesystem_test()
{
	mkdir -p  /mnt/device

	devs=`lsblk -l -o NAME | grep nvme`

	for dev in $devs; do
		parted -s /dev/$dev mklabel msdos
		parted -s /dev/$dev mkpart primary '0%' '100%'
		sleep 1

		for fstype in "ext4" "btrfs" "xfs"; do
			if [ "$fstype" == "ext4" ]; then
				mkfs.${fstype} -F /dev/${dev}
			else
				mkfs.${fstype} -f /dev/${dev}
			fi

			mount /dev/${dev} /mnt/device
			touch /mnt/device/aaa
			rm -rf /mnt/device/aaa
			umount /mnt/device
		done
	done
}

rdma_device_init

set -e

timing_enter fs_test

# Start up the NVMf target in another process
$rootdir/app/nvmf_tgt/nvmf_tgt -c $testdir/../nvmf.conf -t nvmf -t rdma &
nvmfpid=$!

trap "process_core; killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

sleep 10

modprobe -v nvme-rdma

if [ -e "/dev/nvme-fabrics" ]; then
	chmod a+rw /dev/nvme-fabrics
fi

echo 'traddr='$NVMF_FIRST_TARGET_IP',transport=rdma,nr_io_queues=1,trsvcid='$NVMF_PORT',nqn=nqn.2016-06.io.spdk:cnode1' > /dev/nvme-fabrics

# file system test
filesystem_test

rm -f ./local-job0-0-verify.state

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $nvmfpid
timing_exit fs_test
