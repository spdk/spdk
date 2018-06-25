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

	# make sure nbd (network block device) driver is loaded if it is available
	# this ensures that when tests need to use nbd, it will be fully initialized
	modprobe nbd || true
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

# Load the kernel driver
./scripts/setup.sh reset

# Let the kernel discover any filesystems or partitions
sleep 10

if [ $(uname -s) = Linux ]; then
	# OCSSD devices drivers don't support IO issues by kernel so
	# detect OCSSD devices and blacklist them (unbind from any driver).
	# If test scripts want to use this device it needs to do this explicitly.
	#
	# If some OCSSD device is bound to other driver than nvme we won't be able to
	# discover if it is OCSSD or not so load the kernel driver first.


	for dev in $(find /dev -maxdepth 1 -regex '/dev/nvme[0-9]+'); do
		# Send Open Channel 2.0 Geometry opcode "0xe2" - not supported by NVMe device.
		if nvme admin-passthru $dev --namespace-id=1 --data-len=4096  --opcode=0xe2 --read >/dev/null; then
			bdf="$(basename $(readlink -e /sys/class/nvme/${dev#/dev/}/device))"
			echo "INFO: blacklisting OCSSD device: $dev ($bdf)"
			PCI_BLACKLIST+=" $bdf"
			OCSSD_PCI_DEVICES+=" $bdf"
		fi
	done

	export OCSSD_PCI_DEVICES

	# Now, bind blacklisted devices to pci-stub module. This will prevent
	# automatic grabbing these devices when we add device/vendor ID to
	# proper driver.
	if [[ -n "$PCI_BLACKLIST" ]]; then
		PCI_WHITELIST="$PCI_BLACKLIST" \
		PCI_BLACKLIST="" \
		DRIVER_OVERRIDE="pci-stub" \
			./scripts/setup.sh

		# Export our blacklist so it will take effect during next setup.sh
		export PCI_BLACKLIST
	fi
fi

# Delete all leftover lvols and gpt partitions
# Matches both /dev/nvmeXnY on Linux and /dev/nvmeXnsY on BSD
# Filter out nvme with partitions - the "p*" suffix
for dev in $(ls /dev/nvme*n* | grep -v p || true); do
	dd if=/dev/zero of="$dev" bs=1M count=1
done

sync

if [ $(uname -s) = Linux ]; then
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


if [ $SPDK_RUN_FUNCTIONAL_TEST -eq 1 ]; then
	timing_enter lib

	run_test suite test/env/env.sh
	run_test suite test/rpc_client/rpc_client.sh
	run_test suite ./test/json_config/json_config.sh

	if [ $SPDK_TEST_BLOCKDEV -eq 1 ]; then
		run_test suite test/bdev/blockdev.sh
		run_test suite test/bdev/bdev_raid.sh
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
		# breaks the hotplug logic.
		# Temporary workaround for issue #542, annotated for no VM image.
		#if [ $SPDK_RUN_ASAN -eq 0 ]; then
		#	run_test suite test/nvme/hotplug.sh intel
		#fi
	fi

	if [ $SPDK_TEST_IOAT -eq 1 ]; then
		run_test suite test/ioat/ioat.sh
	fi

	timing_exit lib

	if [ $SPDK_TEST_ISCSI -eq 1 ]; then
		run_test suite ./test/iscsi_tgt/iscsi_tgt.sh posix
		if [ $SPDK_RUN_VPP -eq 1 ]; then
			run_test suite ./test/iscsi_tgt/iscsi_tgt.sh vpp
		fi
		run_test suite ./test/spdkcli/iscsi.sh
	fi

	if [ $SPDK_TEST_BLOBFS -eq 1 ]; then
		run_test suite ./test/blobfs/rocksdb/rocksdb.sh
		run_test suite ./test/blobstore/blobstore.sh
	fi

	if [ $SPDK_TEST_NVMF -eq 1 ]; then
		run_test suite ./test/nvmf/nvmf.sh
		run_test suite ./test/spdkcli/nvmf.sh
	fi

	if [ $SPDK_TEST_VHOST -eq 1 ]; then
		run_test suite ./test/vhost/vhost.sh
		report_test_completion "vhost"
	fi

	if [ $SPDK_TEST_LVOL -eq 1 ]; then
		timing_enter lvol
		run_test suite ./test/lvol/lvol.sh --test-cases=all
		run_test suite ./test/blobstore/blob_io_wait/blob_io_wait.sh
		report_test_completion "lvol"
		timing_exit lvol
	fi

	if [ $SPDK_TEST_VHOST_INIT -eq 1 ]; then
		timing_enter vhost_initiator
		run_test suite ./test/vhost/initiator/blockdev.sh
		run_test suite ./test/spdkcli/virtio.sh
		run_test suite ./test/vhost/shared/shared.sh
		report_test_completion "vhost_initiator"
		timing_exit vhost_initiator
	fi

	if [ $SPDK_TEST_PMDK -eq 1 ]; then
		run_test suite ./test/pmem/pmem.sh -x
		run_test suite ./test/spdkcli/pmem.sh
	fi

	if [ $SPDK_TEST_RBD -eq 1 ]; then
		run_test suite ./test/spdkcli/rbd.sh
	fi

	if [ $SPDK_TEST_OCF -eq 1 ]; then
		run_test suite ./test/ocf/ocf.sh
	fi

	if [ $SPDK_TEST_BDEV_FTL -eq 1 ]; then
		run_test suite ./test/ftl/ftl.sh
	fi
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
