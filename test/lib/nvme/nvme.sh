#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/common.sh
source $rootdir/scripts/autotest_common.sh

function get_nvme_name_from_bdf {
	lsblk -d --output NAME
	nvme_devs=$(lsblk -d --output NAME | grep "^nvme") || true
	if [ -z "$nvme_devs" ]; then
		return
	fi
	for dev in $nvme_devs; do
		link_name=$(readlink /sys/block/$dev/device/device) || true
		if [ -z "$link_name" ]; then
			link_name=$(readlink /sys/block/$dev/device)
		fi
		bdf=$(basename "$link_name")
		if [ "$bdf" = "$1" ]; then
			eval "$2=$dev"
			return
		fi
	done
}

timing_enter nvme

if [ `uname` = Linux ]; then
	# check that our setup.sh script does not bind NVMe devices to uio/vfio if they
	# have an active mountpoint
	$rootdir/scripts/setup.sh reset
	# give kernel nvme driver some time to create the block devices before we start looking for them
	sleep 1
	blkname=''
	# first, find an NVMe device that does not have an active mountpoint already;
	# this covers rare case where someone is running this test script on a system
	# that has a mounted NVMe filesystem
	#
	# note: more work probably needs to be done to properly handle devices with multiple
	# namespaces
	for bdf in $(iter_pci_class_code 01 08 02); do
		get_nvme_name_from_bdf "$bdf" blkname
		if [ "$blkname" != "" ]; then
			mountpoints=$(lsblk /dev/$blkname --output MOUNTPOINT -n | wc -w)
			if [ "$mountpoints" = "0" ]; then
				break
			else
				blkname=''
			fi
		fi
	done

	# if we found an NVMe block device without an active mountpoint, create and mount
	# a filesystem on it for purposes of testing the setup.sh script
	if [ "$blkname" != "" ]; then
		parted -s /dev/$blkname mklabel gpt
		# just create a 100MB partition - this tests our ability to detect mountpoints
		# on partitions of the device, not just the device itself;  it also is faster
		# since we don't trim and initialize the whole namespace
		parted -s /dev/$blkname mkpart primary 1 100
		sleep 1
		mkfs.ext4 -F /dev/${blkname}p1
		mkdir -p /tmp/nvmetest
		mount /dev/${blkname}p1 /tmp/nvmetest
		$rootdir/scripts/setup.sh
		driver=$(basename $(readlink /sys/bus/pci/devices/$bdf/driver))
		# check that the nvme driver is still loaded against the device
		if [ "$driver" != "nvme" ]; then
			exit 1
		fi
		umount /tmp/nvmetest
		rmdir /tmp/nvmetest
		# write zeroes to the device to blow away the partition table and filesystem
		dd if=/dev/zero of=/dev/$blkname oflag=direct bs=1M count=1
		$rootdir/scripts/setup.sh
		driver=$(basename $(readlink /sys/bus/pci/devices/$bdf/driver))
		# check that the nvme driver is not loaded against the device
		if [ "$driver" = "nvme" ]; then
			exit 1
		fi
	else
		$rootdir/scripts/setup.sh
	fi
fi

if [ `uname` = Linux ]; then
	start_stub "-s 2048 -i 0 -m 0xF"
	trap "kill_stub; exit 1" SIGINT SIGTERM EXIT
fi

if [ $RUN_NIGHTLY -eq 1 ]; then
	# TODO: temporarily disabled - temperature AER doesn't fire on emulated controllers
	#timing_enter aer
	#$testdir/aer/aer
	#timing_exit aer

	timing_enter reset
	$testdir/reset/reset -q 64 -w write -s 4096 -t 2
	report_test_completion "nightly_nvme_reset"
	timing_exit reset
fi

timing_enter identify
$rootdir/examples/nvme/identify/identify -i 0
for bdf in $(iter_pci_class_code 01 08 02); do
	$rootdir/examples/nvme/identify/identify -r "trtype:PCIe traddr:${bdf}" -i 0
done
timing_exit identify

timing_enter perf
$rootdir/examples/nvme/perf/perf -q 128 -w read -s 12288 -t 1 -LL -i 0
if [ -b /dev/ram0 ]; then
	# Test perf with AIO device
	$rootdir/examples/nvme/perf/perf /dev/ram0 -q 128 -w read -s 12288 -t 1 -LL -i 0
	report_test_completion "nvme_perf"
fi
timing_exit perf

timing_enter reserve
$rootdir/examples/nvme/reserve/reserve
timing_exit reserve

timing_enter hello_world
$rootdir/examples/nvme/hello_world/hello_world
timing_exit

timing_enter deallocated_value
$testdir/deallocated_value/deallocated_value
timing_exit deallocated_value

timing_enter sgl
$testdir/sgl/sgl
timing_exit sgl

timing_enter e2edp
$testdir/e2edp/nvme_dp
timing_exit e2edp

timing_enter overhead
$rootdir/test/lib/nvme/overhead/overhead -s 4096 -t 1 -H
timing_exit overhead

timing_enter arbitration
$rootdir/examples/nvme/arbitration/arbitration -t 3 -i 0
timing_exit arbitration

if [ `uname` = Linux ]; then
	timing_enter multi_secondary
	$rootdir/examples/nvme/perf/perf -i 0 -q 16 -w read -s 4096 -t 3 -c 0x1 &
	pid0=$!
	$rootdir/examples/nvme/perf/perf -i 0 -q 16 -w read -s 4096 -t 3 -c 0x2 &
	pid1=$!
	$rootdir/examples/nvme/perf/perf -i 0 -q 16 -w read -s 4096 -t 3 -c 0x4
	wait $pid0
	wait $pid1
	report_test_completion "nvme_multi_secondary"
	timing_exit multi_secondary
fi

if [ `uname` = Linux ]; then
	trap - SIGINT SIGTERM EXIT
	kill_stub
fi
PLUGIN_DIR=$rootdir/examples/nvme/fio_plugin

if [ -d /usr/src/fio ]; then
	timing_enter fio_plugin
	for bdf in $(iter_pci_class_code 01 08 02); do
		# Only test when ASAN is not enabled. If ASAN is enabled, we cannot test.
		if [ $SPDK_RUN_ASAN -eq 0 ]; then
			LD_PRELOAD=$PLUGIN_DIR/fio_plugin /usr/src/fio/fio $PLUGIN_DIR/example_config.fio --filename="trtype=PCIe traddr=${bdf//:/.} ns=1"
			report_test_completion "bdev_fio"
		fi
		break
	done

	timing_exit fio_plugin
fi

timing_exit nvme
