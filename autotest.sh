#!/usr/bin/env bash

rootdir=$(readlink -f $(dirname $0))
source "$rootdir/test/common/autotest_common.sh"
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

trap "process_core; autotest_cleanup; exit 1" SIGINT SIGTERM EXIT

timing_enter autotest

create_test_list

src=$(readlink -f $(dirname $0))
out=$PWD
cd $src

./scripts/setup.sh status

freebsd_update_contigmem_mod

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

if [ $SPDK_TEST_CRYPTO -eq 1 ]; then
	if grep -q '#define SPDK_CONFIG_IGB_UIO_DRIVER 1' $rootdir/include/spdk/config.h; then
		./scripts/qat_setup.sh igb_uio
	else
		./scripts/qat_setup.sh
	fi
fi

#####################
# Unit Tests
#####################

if [ $SPDK_TEST_UNITTEST -eq 1 ]; then
	timing_enter unittest
	run_test suite ./test/unit/unittest.sh
	report_test_completion "unittest"
	timing_exit unittest
fi

timing_enter lib

if [ $SPDK_TEST_BLOCKDEV -eq 1 ]; then
	run_test suite test/bdev/blockdev.sh
	if [ $(uname -s) = Linux ]; then
		run_test suite test/bdev/bdevjson/json_config.sh
		if modprobe -n nbd; then
			run_test suite test/bdev/nbdjson/json_config.sh
		fi
	fi
fi

if [ $SPDK_TEST_JSON -eq 1 ]; then
	run_test suite test/config_converter/test_converter.sh
fi

if [ $SPDK_TEST_EVENT -eq 1 ]; then
	run_test suite test/event/event.sh
fi

if [ $SPDK_TEST_NVME -eq 1 ]; then
	run_test suite test/nvme/nvme.sh
	if [ $SPDK_TEST_NVME_CLI -eq 1 ]; then
		run_test suite test/nvme/spdk_nvme_cli.sh
	fi
	# Only test hotplug without ASAN enabled. Since if it is
	# enabled, it catches SEGV earlier than our handler which
	# breaks the hotplug logic
	if [ $SPDK_RUN_ASAN -eq 0 ]; then
		run_test suite test/nvme/hotplug.sh intel
	fi
fi

run_test suite test/env/env.sh
run_test suite test/rpc_client/rpc_client.sh

if [ $SPDK_TEST_IOAT -eq 1 ]; then
	run_test suite test/ioat/ioat.sh
fi

timing_exit lib

if [ $SPDK_TEST_ISCSI -eq 1 ]; then
	run_test suite ./test/iscsi_tgt/iscsi_tgt.sh posix
	run_test suite ./test/iscsi_tgt/iscsijson/json_config.sh
	run_test suite ./test/spdkcli/iscsi.sh
fi

if [ $SPDK_TEST_BLOBFS -eq 1 ]; then
	run_test suite ./test/blobfs/rocksdb/rocksdb.sh
	run_test suite ./test/blobstore/blobstore.sh
fi

if [ $SPDK_TEST_NVMF -eq 1 ]; then
	run_test suite ./test/nvmf/nvmf.sh
	run_test suite ./test/nvmf/nvmfjson/json_config.sh
	run_test suite ./test/spdkcli/nvmf.sh
fi

if [ $SPDK_TEST_VHOST -eq 1 ]; then
	timing_enter vhost
	timing_enter negative
	run_test suite ./test/vhost/spdk_vhost.sh --negative
	timing_exit negative

	timing_enter vhost_json_config
	run_test suite ./test/vhost/json_config/json_config.sh
	timing_exit vhost_json_config

	timing_enter vhost_boot
	run_test suite ./test/vhost/spdk_vhost.sh --boot
	timing_exit vhost_boot

	if [ $RUN_NIGHTLY -eq 1 ]; then
		timing_enter integrity_blk
		run_test suite ./test/vhost/spdk_vhost.sh --integrity-blk
		timing_exit integrity_blk

		timing_enter integrity
		run_test suite ./test/vhost/spdk_vhost.sh --integrity
		timing_exit integrity

		timing_enter fs_integrity_scsi
		run_test suite ./test/vhost/spdk_vhost.sh --fs-integrity-scsi
		timing_exit fs_integrity_scsi

		timing_enter fs_integrity_blk
		run_test suite ./test/vhost/spdk_vhost.sh --fs-integrity-blk
		timing_exit fs_integrity_blk

		timing_enter integrity_lvol_scsi_nightly
		run_test suite ./test/vhost/spdk_vhost.sh --integrity-lvol-scsi-nightly
		timing_exit integrity_lvol_scsi_nightly

		timing_enter integrity_lvol_blk_nightly
		run_test suite ./test/vhost/spdk_vhost.sh --integrity-lvol-blk-nightly
		timing_exit integrity_lvol_blk_nightly

		timing_enter vhost_migration
		run_test suite ./test/vhost/spdk_vhost.sh --migration
		timing_exit vhost_migration

		# timing_enter readonly
		# run_test suite ./test/vhost/spdk_vhost.sh --readonly
		# timing_exit readonly
	fi

	timing_enter integrity_lvol_scsi
	run_test suite ./test/vhost/spdk_vhost.sh --integrity-lvol-scsi
	timing_exit integrity_lvol_scsi

	timing_enter integrity_lvol_blk
	run_test suite ./test/vhost/spdk_vhost.sh --integrity-lvol-blk
	timing_exit integrity_lvol_blk

	timing_enter spdk_cli
	run_test suite ./test/spdkcli/vhost.sh
	timing_exit spdk_cli

	timing_exit vhost
fi

if [ $SPDK_TEST_LVOL -eq 1 ]; then
	timing_enter lvol
	test_cases="1,50,51,52,53,100,101,102,150,200,201,250,251,252,253,254,255,"
	test_cases+="300,301,450,451,452,550,551,552,553,"
	test_cases+="600,601,650,651,652,654,655,"
	test_cases+="700,701,702,750,751,752,753,754,755,756,757,758,759,"
	test_cases+="800,801,802,803,804,10000"
	run_test suite ./test/lvol/lvol.sh --test-cases=$test_cases
	run_test suite ./test/blobstore/blob_io_wait/blob_io_wait.sh
	report_test_completion "lvol"
	timing_exit lvol
fi

if [ $SPDK_TEST_VHOST_INIT -eq 1 ]; then
	run_test suite ./test/vhost/initiator/blockdev.sh
	run_test suite ./test/vhost/initiator/json_config.sh
	run_test suite ./test/spdkcli/virtio.sh
	report_test_completion "vhost_initiator"
fi

if [ $SPDK_TEST_PMDK -eq 1 ]; then
	run_test suite ./test/pmem/pmem.sh -x
	run_test suite ./test/pmem/json_config/json_config.sh
	run_test suite ./test/spdkcli/pmem.sh
fi

if [ $SPDK_TEST_RBD -eq 1 ]; then
	run_test suite ./test/bdev/bdevjson/rbd_json_config.sh
	run_test suite ./test/spdkcli/rbd.sh
fi

timing_enter cleanup
autotest_cleanup
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
