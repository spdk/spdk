#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

function nvme_identify() {
	$SPDK_EXAMPLE_DIR/identify -i 0
	for bdf in $(get_nvme_bdfs); do
		$SPDK_EXAMPLE_DIR/identify -r "trtype:PCIe traddr:${bdf}" -i 0
	done
	timing_exit identify
}

function nvme_perf() {
	# enable no shutdown notification option
	$SPDK_EXAMPLE_DIR/perf -q 128 -w read -o 12288 -t 1 -LL -i 0 -N
	$SPDK_EXAMPLE_DIR/perf -q 128 -w write -o 12288 -t 1 -LL -i 0
	if [ -b /dev/ram0 ]; then
		# Test perf with AIO device
		$SPDK_EXAMPLE_DIR/perf /dev/ram0 -q 128 -w read -o 12288 -t 1 -LL -i 0
	fi
}

function nvme_fio_test() {
	PLUGIN_DIR=$rootdir/examples/nvme/fio_plugin
	ran_fio=false
	for bdf in $(get_nvme_bdfs); do
		if $SPDK_EXAMPLE_DIR/identify -r "trtype:PCIe traddr:${bdf}" | grep -E "^Number of Namespaces" - | grep -q "0" -; then
			continue
		fi
		fio_nvme $PLUGIN_DIR/example_config.fio --filename="trtype=PCIe traddr=${bdf//:/.}"
		ran_fio=true
	done
	$ran_fio || (echo "No valid NVMe drive found. Failing test." && false)
}

function nvme_multi_secondary() {
	$SPDK_EXAMPLE_DIR/perf -i 0 -q 16 -w read -o 4096 -t 3 -c 0x1 &
	pid0=$!
	$SPDK_EXAMPLE_DIR/perf -i 0 -q 16 -w read -o 4096 -t 3 -c 0x2 &
	pid1=$!
	$SPDK_EXAMPLE_DIR/perf -i 0 -q 16 -w read -o 4096 -t 3 -c 0x4
	wait $pid0
	wait $pid1
}

if [ $(uname) = Linux ]; then
	# check that our setup.sh script does not bind NVMe devices to uio/vfio if they
	# have an active mountpoint
	$rootdir/scripts/setup.sh reset
	blkname=''
	# first, find an NVMe device that does not have an active mountpoint already;
	# this covers rare case where someone is running this test script on a system
	# that has a mounted NVMe filesystem
	#
	# note: more work probably needs to be done to properly handle devices with multiple
	# namespaces
	for bdf in $(get_nvme_bdfs); do
		for name in $(get_nvme_name_from_bdf $bdf); do
			if [ "$name" != "" ]; then
				mountpoints=$(lsblk /dev/$name --output MOUNTPOINT -n | wc -w)
				if [ "$mountpoints" = "0" ]; then
					blkname=$name
					break 2
				fi
			fi
		done
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
		sleep 1
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

if [ $(uname) = Linux ]; then
	trap "kill_stub -9; exit 1" SIGINT SIGTERM EXIT
	start_stub "-s 4096 -i 0 -m 0xE"
fi

run_test "nvme_reset" $testdir/reset/reset -q 64 -w write -s 4096 -t 5
run_test "nvme_identify" nvme_identify
run_test "nvme_perf" nvme_perf
run_test "nvme_hello_world" $SPDK_EXAMPLE_DIR/hello_world
run_test "nvme_deallocated_value" $testdir/deallocated_value/deallocated_value
run_test "nvme_sgl" $testdir/sgl/sgl
run_test "nvme_e2edp" $testdir/e2edp/nvme_dp
run_test "nvme_reserve" $testdir/reserve/reserve
run_test "nvme_err_injection" $testdir/err_injection/err_injection
run_test "nvme_overhead" $testdir/overhead/overhead -s 4096 -t 1 -H
run_test "nvme_arbitration" $SPDK_EXAMPLE_DIR/arbitration -t 3 -i 0

if [ $(uname) != "FreeBSD" ]; then
	run_test "nvme_startup" $testdir/startup/startup -t 1000000
	run_test "nvme_multi_secondary" nvme_multi_secondary
	trap - SIGINT SIGTERM EXIT
	kill_stub
fi

if [[ $CONFIG_FIO_PLUGIN == y ]]; then
	run_test "nvme_fio" nvme_fio_test
fi
