#!/usr/bin/env bash

rootdir=$(readlink -f $(dirname $0))
source "$rootdir/scripts/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

set -xe

if [ $EUID -ne 0 ]; then
	echo "$0 must be run as root"
	exit 1
fi

if [ $(uname -s) = Linux ]; then
	# set core_pattern to a known value to avoid ABRT, systemd-coredump, etc.
	echo "core" > /proc/sys/kernel/core_pattern
fi

trap "process_core; $rootdir/scripts/setup.sh reset; exit 1" SIGINT SIGTERM EXIT

timing_enter autotest

src=$(readlink -f $(dirname $0))
out=$PWD
cd $src

if hash lcov; then
	# setup output dir for unittest.sh
	export UT_COVERAGE=$out/ut_coverage
	export LCOV_OPTS="
		--rc lcov_branch_coverage=1
		--rc lcov_function_coverage=1
		--rc genhtml_branch_coverage=1
		--rc genhtml_function_coverage=1
		--rc genhtml_legend=1
		--rc geninfo_all_blocks=1
		"
	export LCOV="lcov $LCOV_OPTS --no-external"
	# zero out coverage data
	$LCOV -q -c -i -t "Baseline" -d $src -o cov_base.info
fi

# Make sure the disks are clean (no leftover partition tables)
timing_enter cleanup
if [ $(uname -s) = Linux ]; then
	# Load the kernel driver
	./scripts/setup.sh reset

	# Let the kernel discover any filesystems or partitions
	sleep 10

	# Delete all partitions on NVMe devices
	devs=`lsblk -l -o NAME | grep nvme | grep -v p` || true
	for dev in $devs; do
		parted -s /dev/$dev mklabel msdos
	done
fi
timing_exit cleanup

# set up huge pages
timing_enter afterboot
./scripts/setup.sh
timing_exit afterboot

timing_enter nvmf_setup
rdma_device_init
timing_exit nvmf_setup

timing_enter rbd_setup
rbd_setup
timing_exit rbd_setup

#####################
# Unit Tests
#####################

if [ $SPDK_TEST_UNITTEST -eq 1 ]; then
	timing_enter unittest
	run_test ./unittest.sh
	timing_exit unittest
fi

timing_enter lib

if [ $SPDK_TEST_BLOCKDEV -eq 1 ]; then
	run_test test/lib/bdev/blockdev.sh
fi

if [ $SPDK_TEST_EVENT -eq 1 ]; then
	run_test test/lib/event/event.sh
fi

if [ $SPDK_TEST_NVME -eq 1 ]; then
	run_test test/lib/nvme/nvme.sh
	# Only test hotplug without ASAN enabled. Since if it is
	# enabled, it catches SEGV earlier than our handler which
	# breaks the hotplug logic
	if [ $SPDK_RUN_ASAN -eq 0 ]; then
		run_test test/lib/nvme/hotplug.sh intel
	fi
fi

run_test test/lib/env/env.sh

if [ $SPDK_TEST_IOAT -eq 1 ]; then
	run_test test/lib/ioat/ioat.sh
fi

timing_exit lib

if [ $SPDK_TEST_ISCSI -eq 1 ]; then
	run_test ./test/iscsi_tgt/iscsi_tgt.sh
fi

if [ $SPDK_TEST_BLOBFS -eq 1 ]; then
	run_test ./test/blobfs/rocksdb/rocksdb.sh
fi

if [ $SPDK_TEST_NVMF -eq 1 ]; then
	run_test ./test/nvmf/nvmf.sh
fi

if [ $SPDK_TEST_VHOST -eq 1 ]; then
	timing_enter vhost
	run_test ./test/vhost/spdk_vhost.sh --integrity-blk
	run_test ./test/vhost/spdk_vhost.sh --integrity
	timing_exit vhost
fi

timing_enter cleanup
rbd_cleanup
./scripts/setup.sh reset
./scripts/build_kmod.sh clean
timing_exit cleanup

timing_exit autotest
chmod a+r $output_dir/timing.txt

trap - SIGINT SIGTERM EXIT

# catch any stray core files
process_core

if hash lcov; then
	# generate coverage data and combine with baseline
	$LCOV -q -c -d $src -t "$(hostname)" -o cov_test.info
	$LCOV -q -a cov_base.info -a cov_test.info -o $out/cov_total.info
	$LCOV -q -r $out/cov_total.info '*/dpdk/*' -o $out/cov_total.info
	$LCOV -q -r $out/cov_total.info '/usr/*' -o $out/cov_total.info
	git clean -f "*.gcda"
	rm -f cov_base.info cov_test.info OLD_STDOUT OLD_STDERR
fi
