#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation
#  All rights reserved.
#

rootdir=$(readlink -f $(dirname $0))

# In autotest_common.sh all tests are disabled by default.
# If the configuration of tests is not provided, no tests will be carried out.
if [[ ! -f $1 ]]; then
	echo "ERROR: SPDK test configuration not specified"
	exit 1
fi

# Autotest.sh, as part of autorun.sh, runs in a different
# shell process than autobuild.sh. Use helper file to pass
# over env variable containing libraries paths.
if [[ -e /tmp/spdk-ld-path ]]; then
	source /tmp/spdk-ld-path
fi

source "$1"
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

if [ $EUID -ne 0 ]; then
	echo "$0 must be run as root"
	exit 1
fi

if [ $(uname -s) = Linux ]; then
	old_core_pattern=$(< /proc/sys/kernel/core_pattern)
	mkdir -p "$output_dir/coredumps"
	# Set core_pattern to a known value to avoid ABRT, systemd-coredump, etc.
	# Dump the $output_dir path to a file so collector can pick it up while executing.
	# We don't set in in the core_pattern command line because of the string length limitation
	# of 128 bytes. See 'man core 5' for details.
	echo "|$rootdir/scripts/core-collector.sh %P %s %t" > /proc/sys/kernel/core_pattern
	echo "$output_dir/coredumps" > "$rootdir/.coredump_path"

	# make sure nbd (network block device) driver is loaded if it is available
	# this ensures that when tests need to use nbd, it will be fully initialized
	modprobe nbd || true

	if udevadm=$(type -P udevadm); then
		"$udevadm" monitor --property &> "$output_dir/udev.log" &
		udevadm_pid=$!
	fi
fi

trap "autotest_cleanup || :; exit 1" SIGINT SIGTERM EXIT

timing_enter autotest

create_test_list

src=$(readlink -f $(dirname $0))
out=$output_dir
cd $src

freebsd_update_contigmem_mod
freebsd_set_maxsock_buf

# lcov takes considerable time to process clang coverage.
# Disabling lcov allow us to do this.
# More information: https://github.com/spdk/spdk/issues/1693
CC_TYPE=$(grep CC_TYPE mk/cc.mk)
if hash lcov && ! [[ "$CC_TYPE" == *"clang"* ]]; then
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
	$LCOV -q -c -i -t "Baseline" -d $src -o $out/cov_base.info
fi

# Make sure the disks are clean (no leftover partition tables)
timing_enter cleanup
# Remove old domain socket pathname just in case
rm -f /var/tmp/spdk*.sock

# Load the kernel driver
./scripts/setup.sh reset

get_zoned_devs

if ((${#zoned_devs[@]} > 0)); then
	# FIXME: For now make sure zoned devices are tested on-demand by
	# a designated tests instead of falling into any other. The main
	# concern here are fio workloads where specific configuration
	# must be in place for it to work with the zoned device.
	export PCI_BLOCKED="${zoned_devs[*]}"
	export PCI_ZONED="${zoned_devs[*]}"
fi

# Delete all leftover lvols and gpt partitions
# Matches both /dev/nvmeXnY on Linux and /dev/nvmeXnsY on BSD
# Filter out nvme with partitions - the "p*" suffix
for dev in $(ls /dev/nvme*n* | grep -v p || true); do
	# Skip zoned devices as non-sequential IO will always fail
	[[ -z ${zoned_devs["${dev##*/}"]} ]] || continue
	if ! block_in_use "$dev"; then
		dd if=/dev/zero of="$dev" bs=1M count=1
	fi
done

sync

if [ $(uname -s) = Linux ]; then
	run_test "setup.sh" "$rootdir/test/setup/test-setup.sh"
fi

./scripts/setup.sh status

if [[ $(uname -s) == Linux ]]; then
	# Revert NVMe namespaces to default state
	nvme_namespace_revert
fi

timing_exit cleanup

# set up huge pages
timing_enter afterboot
./scripts/setup.sh
timing_exit afterboot

# Revert existing OPAL to factory settings that may have been left from earlier failed tests.
# This ensures we won't hit any unexpected failures due to NVMe SSDs being locked.
opal_revert_cleanup

#####################
# Unit Tests
#####################

if [ $SPDK_TEST_UNITTEST -eq 1 ]; then
	run_test "unittest" ./test/unit/unittest.sh
	run_test "env" test/env/env.sh
fi

if [ $SPDK_RUN_FUNCTIONAL_TEST -eq 1 ]; then
	if [[ $SPDK_TEST_CRYPTO -eq 1 || $SPDK_TEST_VBDEV_COMPRESS -eq 1 ]]; then
		if [[ $SPDK_TEST_USE_IGB_UIO -eq 1 ]]; then
			./scripts/qat_setup.sh igb_uio
		else
			./scripts/qat_setup.sh
		fi
	fi
	timing_enter lib

	run_test "rpc" test/rpc/rpc.sh
	run_test "rpc_client" test/rpc_client/rpc_client.sh
	run_test "json_config" ./test/json_config/json_config.sh
	run_test "json_config_extra_key" ./test/json_config/json_config_extra_key.sh
	run_test "alias_rpc" test/json_config/alias_rpc/alias_rpc.sh
	run_test "spdkcli_tcp" test/spdkcli/tcp.sh
	run_test "dpdk_mem_utility" test/dpdk_memory_utility/test_dpdk_mem_info.sh
	run_test "event" test/event/event.sh
	run_test "thread" test/thread/thread.sh
	run_test "accel" test/accel/accel.sh
	run_test "app_cmdline" test/app/cmdline.sh

	if [ $SPDK_TEST_BLOCKDEV -eq 1 ]; then
		run_test "blockdev_general" test/bdev/blockdev.sh
		run_test "bdev_raid" test/bdev/bdev_raid.sh
		run_test "bdevperf_config" test/bdev/bdevperf/test_config.sh
		if [[ $(uname -s) == Linux ]]; then
			run_test "reactor_set_interrupt" test/interrupt/reactor_set_interrupt.sh
			run_test "reap_unregistered_poller" test/interrupt/reap_unregistered_poller.sh
		fi
	fi

	if [[ $(uname -s) == Linux ]]; then
		if [[ $SPDK_TEST_BLOCKDEV -eq 1 || $SPDK_TEST_URING -eq 1 ]]; then
			# The crypto job also includes the SPDK_TEST_BLOCKDEV in its configuration hence the
			# dd tests are executed there as well. However, these tests can take a significant
			# amount of time to complete (up to 4min) on a physical system leading to a potential
			# job timeout. Avoid that by skipping these tests - this should not affect the coverage
			# since dd tests are still run as part of the vg jobs.
			if [[ $SPDK_TEST_CRYPTO -eq 0 ]]; then
				run_test "spdk_dd" test/dd/dd.sh
			fi
		fi
	fi

	if [ $SPDK_TEST_NVME -eq 1 ]; then
		run_test "blockdev_nvme" test/bdev/blockdev.sh "nvme"
		if [[ $(uname -s) == Linux ]]; then
			run_test "blockdev_nvme_gpt" test/bdev/blockdev.sh "gpt"
		fi
		run_test "nvme" test/nvme/nvme.sh
		if [[ $SPDK_TEST_NVME_PMR -eq 1 ]]; then
			run_test "nvme_pmr" test/nvme/nvme_pmr.sh
		fi
		if [[ $SPDK_TEST_NVME_SCC -eq 1 ]]; then
			run_test "nvme_scc" test/nvme/nvme_scc.sh
		fi
		if [[ $SPDK_TEST_NVME_BP -eq 1 ]]; then
			run_test "nvme_bp" test/nvme/nvme_bp.sh
		fi
		if [[ $SPDK_TEST_NVME_CUSE -eq 1 ]]; then
			run_test "nvme_cuse" test/nvme/cuse/nvme_cuse.sh
		fi
		if [[ $SPDK_TEST_NVME_CMB -eq 1 ]]; then
			run_test "nvme_cmb" test/nvme/cmb/cmb.sh
		fi

		if [[ $SPDK_TEST_NVME_ZNS -eq 1 ]]; then
			run_test "nvme_zns" test/nvme/zns/zns.sh
		fi

		run_test "nvme_rpc" test/nvme/nvme_rpc.sh
		run_test "nvme_rpc_timeouts" test/nvme/nvme_rpc_timeouts.sh
		# Only test hotplug without ASAN enabled. Since if it is
		# enabled, it catches SEGV earlier than our handler which
		# breaks the hotplug logic.
		if [ $SPDK_RUN_ASAN -eq 0 ] && [ $(uname -s) = Linux ]; then
			run_test "sw_hotplug" test/nvme/sw_hotplug.sh
		fi

		if [[ $SPDK_TEST_XNVME -eq 1 ]]; then
			run_test "nvme_xnvme" test/nvme/xnvme/xnvme.sh
			run_test "blockdev_xnvme" test/bdev/blockdev.sh "xnvme"
			# Run ublk with xnvme since they have similar kernel dependencies
			run_test "ublk" test/ublk/ublk.sh
		fi
	fi

	if [ $SPDK_TEST_IOAT -eq 1 ]; then
		run_test "ioat" test/ioat/ioat.sh
	fi

	timing_exit lib

	if [ $SPDK_TEST_ISCSI -eq 1 ]; then
		run_test "iscsi_tgt" ./test/iscsi_tgt/iscsi_tgt.sh
		run_test "spdkcli_iscsi" ./test/spdkcli/iscsi.sh

		# Run raid spdkcli test under iSCSI since blockdev tests run on systems that can't run spdkcli yet
		run_test "spdkcli_raid" test/spdkcli/raid.sh
	fi

	if [ $SPDK_TEST_BLOBFS -eq 1 ]; then
		run_test "rocksdb" ./test/blobfs/rocksdb/rocksdb.sh
		run_test "blobstore" ./test/blobstore/blobstore.sh
		run_test "blobstore_grow" ./test/blobstore/blobstore_grow/blobstore_grow.sh
		run_test "blobfs" ./test/blobfs/blobfs.sh
		run_test "hello_blob" $SPDK_EXAMPLE_DIR/hello_blob \
			examples/blob/hello_world/hello_blob.json
	fi

	if [ $SPDK_TEST_NVMF -eq 1 ]; then
		export NET_TYPE
		# The NVMe-oF run test cases are split out like this so that the parser that compiles the
		# list of all tests can properly differentiate them. Please do not merge them into one line.
		if [ "$SPDK_TEST_NVMF_TRANSPORT" = "rdma" ]; then
			run_test "nvmf_rdma" ./test/nvmf/nvmf.sh --transport=$SPDK_TEST_NVMF_TRANSPORT
			run_test "spdkcli_nvmf_rdma" ./test/spdkcli/nvmf.sh --transport=$SPDK_TEST_NVMF_TRANSPORT
		elif [ "$SPDK_TEST_NVMF_TRANSPORT" = "tcp" ]; then
			run_test "nvmf_tcp" ./test/nvmf/nvmf.sh --transport=$SPDK_TEST_NVMF_TRANSPORT
			if [[ $SPDK_TEST_URING -eq 0 ]]; then
				run_test "spdkcli_nvmf_tcp" ./test/spdkcli/nvmf.sh --transport=$SPDK_TEST_NVMF_TRANSPORT
				run_test "nvmf_identify_passthru" test/nvmf/target/identify_passthru.sh --transport=$SPDK_TEST_NVMF_TRANSPORT
			fi
			run_test "nvmf_dif" test/nvmf/target/dif.sh
			run_test "nvmf_abort_qd_sizes" test/nvmf/target/abort_qd_sizes.sh
		elif [ "$SPDK_TEST_NVMF_TRANSPORT" = "fc" ]; then
			run_test "nvmf_fc" ./test/nvmf/nvmf.sh --transport=$SPDK_TEST_NVMF_TRANSPORT
			run_test "spdkcli_nvmf_fc" ./test/spdkcli/nvmf.sh
		else
			echo "unknown NVMe transport, please specify rdma, tcp, or fc."
			exit 1
		fi
	fi

	if [ $SPDK_TEST_VHOST -eq 1 ]; then
		run_test "vhost" ./test/vhost/vhost.sh
	fi

	if [ $SPDK_TEST_VFIOUSER_QEMU -eq 1 ]; then
		run_test "vfio_user_qemu" ./test/vfio_user/vfio_user.sh
	fi

	if [ $SPDK_TEST_LVOL -eq 1 ]; then
		run_test "lvol" ./test/lvol/lvol.sh
		run_test "blob_io_wait" ./test/blobstore/blob_io_wait/blob_io_wait.sh
	fi

	if [ $SPDK_TEST_VHOST_INIT -eq 1 ]; then
		timing_enter vhost_initiator
		run_test "vhost_blockdev" ./test/vhost/initiator/blockdev.sh
		run_test "spdkcli_virtio" ./test/spdkcli/virtio.sh
		run_test "vhost_shared" ./test/vhost/shared/shared.sh
		run_test "vhost_fuzz" ./test/vhost/fuzz/fuzz.sh
		timing_exit vhost_initiator
	fi

	if [ $SPDK_TEST_PMDK -eq 1 ]; then
		run_test "blockdev_pmem" ./test/bdev/blockdev.sh "pmem"
		run_test "pmem" ./test/pmem/pmem.sh -x
		run_test "spdkcli_pmem" ./test/spdkcli/pmem.sh
	fi

	if [ $SPDK_TEST_RBD -eq 1 ]; then
		run_test "blockdev_rbd" ./test/bdev/blockdev.sh "rbd"
		run_test "spdkcli_rbd" ./test/spdkcli/rbd.sh
	fi

	if [ $SPDK_TEST_OCF -eq 1 ]; then
		run_test "ocf" ./test/ocf/ocf.sh
	fi

	if [ $SPDK_TEST_FTL -eq 1 ]; then
		run_test "ftl" ./test/ftl/ftl.sh
	fi

	if [ $SPDK_TEST_VMD -eq 1 ]; then
		run_test "vmd" ./test/vmd/vmd.sh
	fi

	if [ $SPDK_TEST_VBDEV_COMPRESS -eq 1 ]; then
		run_test "compress_compdev" ./test/compress/compress.sh "compdev"
		run_test "compress_isal" ./test/compress/compress.sh "isal"
	fi

	if [ $SPDK_TEST_OPAL -eq 1 ]; then
		run_test "nvme_opal" ./test/nvme/nvme_opal.sh
	fi

	if [ $SPDK_TEST_CRYPTO -eq 1 ]; then
		run_test "blockdev_crypto_aesni" ./test/bdev/blockdev.sh "crypto_aesni"
		run_test "blockdev_crypto_sw" ./test/bdev/blockdev.sh "crypto_sw"
		# Proceed with the test only if QAT devices are in place
		if [[ $(lspci -d:37c8) ]]; then
			run_test "blockdev_crypto_qat" ./test/bdev/blockdev.sh "crypto_qat"
		fi
	fi

	if [[ $SPDK_TEST_SCHEDULER -eq 1 ]]; then
		run_test "scheduler" ./test/scheduler/scheduler.sh
	fi

	if [[ $SPDK_TEST_SMA -eq 1 ]]; then
		run_test "sma" ./test/sma/sma.sh
	fi

	if [[ $SPDK_TEST_FUZZER -eq 1 ]]; then
		run_test "llvm_fuzz" test/fuzz/llvm.sh
	fi

	if [[ $SPDK_TEST_RAID5 -eq 1 ]]; then
		run_test "blockdev_raid5f" ./test/bdev/blockdev.sh "raid5f"
	fi
fi

trap - SIGINT SIGTERM EXIT

timing_enter cleanup
autotest_cleanup
timing_exit cleanup

timing_exit autotest
chmod a+r $output_dir/timing.txt

[[ -f "$output_dir/udev.log" ]] && rm -f "$output_dir/udev.log"

if hash lcov && ! [[ "$CC_TYPE" == *"clang"* ]]; then
	# generate coverage data and combine with baseline
	$LCOV -q -c -d $src -t "$(hostname)" -o $out/cov_test.info
	$LCOV -q -a $out/cov_base.info -a $out/cov_test.info -o $out/cov_total.info
	$LCOV -q -r $out/cov_total.info '*/dpdk/*' -o $out/cov_total.info
	$LCOV -q -r $out/cov_total.info '/usr/*' -o $out/cov_total.info
	$LCOV -q -r $out/cov_total.info '*/examples/vmd/*' -o $out/cov_total.info
	$LCOV -q -r $out/cov_total.info '*/app/spdk_lspci/*' -o $out/cov_total.info
	owner=$(stat -c "%U" .)
	sudo -u $owner git clean -f "*.gcda"
	rm -f cov_base.info cov_test.info OLD_STDOUT OLD_STDERR
fi
