#!/usr/bin/env bash

rootdir=$(readlink -f $(dirname $0))

# In autotest_common.sh all tests are disabled by default.
# If the configuration of tests is not provided, no tests will be carried out.
if [[ ! -f $1 ]]; then
	echo "ERROR: SPDK test configuration not specified"
	exit 1
fi

source "$1"
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

if [[ $SPDK_TEST_CRYPTO -eq 1 || $SPDK_TEST_REDUCE -eq 1 ]]; then
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

	if [ $SPDK_TEST_OCF -eq 1 ]; then
		run_test suite ./test/ocf/ocf.sh
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
