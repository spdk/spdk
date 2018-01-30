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

./scripts/setup.sh status

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
	# Print lcov version to log
	$LCOV -v
	# zero out coverage data
	$LCOV -q -c -i -t "Baseline" -d $src -o cov_base.info
fi

# Make sure the disks are clean (no leftover partition tables)
timing_enter cleanup
# Remove old domain socket pathname just in case
rm -f /var/tmp/spdk*.sock
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

	# Load RAM disk driver if available
	modprobe brd || true
fi
timing_exit cleanup

# set up huge pages
timing_enter afterboot
./scripts/setup.sh
timing_exit afterboot

timing_enter nvmf_setup
rdma_device_init
timing_exit nvmf_setup

if [ $SPDK_TEST_RBD -eq 1 ]; then
	timing_enter rbd_setup
	rbd_setup
	timing_exit rbd_setup
fi

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
	run_test ./test/blobstore/blobstore.sh
fi

if [ $SPDK_TEST_NVMF -eq 1 ]; then
	run_test ./test/nvmf/nvmf.sh
fi

if [ $SPDK_TEST_VHOST -eq 1 ]; then
	timing_enter vhost
	timing_enter negative
	run_test ./test/vhost/spdk_vhost.sh --negative
	timing_exit negative

	if [ $RUN_NIGHTLY -eq 1 ]; then
		timing_enter integrity_blk
		run_test ./test/vhost/spdk_vhost.sh --integrity-blk
		timing_exit integrity_blk

		timing_enter integrity
		run_test ./test/vhost/spdk_vhost.sh --integrity
		timing_exit integrity

		timing_enter readonly
		run_test ./test/vhost/spdk_vhost.sh --readonly
		timing_exit readonly

		# timing_enter fs_integrity_scsi
		# run_test ./test/vhost/spdk_vhost.sh --fs-integrity-scsi
		# timing_exit fs_integrity_scsi

		# timing_enter fs_integrity_blk
		# run_test ./test/vhost/spdk_vhost.sh --fs-integrity-blk
		# timing_exit fs_integrity_blk

		timing_enter integrity_lvol_scsi_nightly
		run_test ./test/vhost/spdk_vhost.sh --integrity-lvol-scsi-nightly
		timing_exit integrity_lvol_scsi_nightly

		timing_enter integrity_lvol_blk_nightly
		run_test ./test/vhost/spdk_vhost.sh --integrity-lvol-blk-nightly
		timing_exit integrity_lvol_blk_nightly
	fi

	timing_enter integrity_lvol_scsi
	run_test ./test/vhost/spdk_vhost.sh --integrity-lvol-scsi
	timing_exit integrity_lvol_scsi

	timing_enter integrity_lvol_blk
	run_test ./test/vhost/spdk_vhost.sh --integrity-lvol-blk
	timing_exit integrity_lvol_blk

	timing_enter vhost_migration
	run_test ./test/vhost/spdk_vhost.sh --migration
	timing_exit vhost_migration

	timing_exit vhost
fi

if [ $SPDK_TEST_LVOL -eq 1 ]; then
	timing_enter lvol
	test_cases="1,50,51,52,53,100,101,102,250,251,252,253,255,"
	test_cases+="300,301,450,451,452,550,600,601,650,651,652,654,655,"
	test_cases+="700,701,800,801,802,803,804,10000"
	run_test ./test/lvol/lvol.sh --test-cases=$test_cases
	timing_exit lvol
fi

if [ $SPDK_TEST_VHOST_INIT -eq 1 ]; then
	run_test ./test/vhost/initiator/blockdev.sh
fi

if [ $SPDK_TEST_NVML -eq 1 ]; then
	run_test ./test/pmem/pmem.sh
fi

timing_enter cleanup
if [ $SPDK_TEST_RBD -eq 1 ]; then
	rbd_cleanup
fi
./scripts/setup.sh reset
if [ $SPDK_BUILD_IOAT_KMOD -eq 1 ]; then
	./scripts/build_kmod.sh clean
fi
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
