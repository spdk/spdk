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

init_linux_env

start_monitor_resources

trap "autotest_cleanup || :; exit 1" SIGINT SIGTERM EXIT

timing_enter autotest

create_test_list

src=$(readlink -f $(dirname $0))
out=$output_dir
cd $src

freebsd_update_mods
freebsd_set_maxsock_buf

if [[ $CONFIG_COVERAGE == y ]]; then
	# Print lcov version to log
	$LCOV --version
	# zero out coverage data
	$LCOV -q -c --no-external -i -t "Baseline" -d $src -o $out/cov_base.info
fi

# Make sure the disks are clean (no leftover partition tables)
timing_enter pre_cleanup
# Remove old domain socket pathname just in case
rm -f /var/tmp/spdk*.sock

# Load the kernel driver
$rootdir/scripts/setup.sh reset

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
for dev in /dev/nvme*n!(*p*); do
	# Skip zoned devices as non-sequential IO will always fail
	[[ -z ${zoned_devs["${dev##*/}"]} ]] || continue
	if ! block_in_use "$dev"; then
		dd if=/dev/zero of="$dev" bs=1M count=1
	fi
done

sync

if ! xtrace_disable_per_cmd reap_spdk_processes; then
	echo "WARNING: Lingering SPDK processes were detected. Testing environment may be unstable" >&2
fi

if [[ $(uname -s) == Linux && $SPDK_TEST_SETUP -eq 1 ]]; then
	run_test "setup.sh" "$rootdir/test/setup/test-setup.sh"
fi

$rootdir/scripts/setup.sh status

if [[ $(uname -s) == Linux ]]; then
	# Revert NVMe namespaces to default state
	nvme_namespace_revert
fi

timing_exit pre_cleanup

# set up huge pages
timing_enter afterboot
$rootdir/scripts/setup.sh
timing_exit afterboot

# Revert existing OPAL to factory settings that may have been left from earlier failed tests.
# This ensures we won't hit any unexpected failures due to NVMe SSDs being locked.
opal_revert_cleanup

#####################
# Unit Tests
#####################

if [ $SPDK_TEST_UNITTEST -eq 1 ]; then
	run_test "unittest" $rootdir/test/unit/unittest.sh
fi

if [ $SPDK_RUN_FUNCTIONAL_TEST -eq 1 ]; then
	if [[ $SPDK_TEST_CRYPTO -eq 1 ]]; then
		if [[ $SPDK_TEST_USE_IGB_UIO -eq 1 ]]; then
			$rootdir/scripts/qat_setup.sh igb_uio
		else
			$rootdir/scripts/qat_setup.sh
		fi
	fi
	timing_enter lib

	if [[ $SPDK_TEST_URING -eq 1 ]]; then
		export SPDK_SOCK_IMPL_DEFAULT="uring"
	fi

	run_test "env" $rootdir/test/env/env.sh
	run_test "rpc" $rootdir/test/rpc/rpc.sh
	run_test "skip_rpc" $rootdir/test/rpc/skip_rpc.sh
	run_test "rpc_client" $rootdir/test/rpc_client/rpc_client.sh
	run_test "json_config" $rootdir/test/json_config/json_config.sh
	run_test "json_config_extra_key" $rootdir/test/json_config/json_config_extra_key.sh
	run_test "alias_rpc" $rootdir/test/json_config/alias_rpc/alias_rpc.sh

	if [[ $SPDK_JSONRPC_GO_CLIENT -eq 0 ]]; then
		run_test "spdkcli_tcp" $rootdir/test/spdkcli/tcp.sh
	fi

	run_test "dpdk_mem_utility" $rootdir/test/dpdk_memory_utility/test_dpdk_mem_info.sh
	run_test "event" $rootdir/test/event/event.sh
	run_test "thread" $rootdir/test/thread/thread.sh

	if [[ $SPDK_TEST_ACCEL -eq 1 ]]; then
		run_test "accel" $rootdir/test/accel/accel.sh
		run_test "accel_rpc" $rootdir/test/accel/accel_rpc.sh
	fi

	run_test "app_cmdline" $rootdir/test/app/cmdline.sh
	run_test "version" $rootdir/test/app/version.sh

	if [ $SPDK_TEST_BLOCKDEV -eq 1 ]; then
		run_test "blockdev_general" $rootdir/test/bdev/blockdev.sh
		run_test "bdevperf_config" $rootdir/test/bdev/bdevperf/test_config.sh
		if [[ $(uname -s) == Linux ]]; then
			run_test "reactor_set_interrupt" $rootdir/test/interrupt/reactor_set_interrupt.sh
			run_test "reap_unregistered_poller" $rootdir/test/interrupt/reap_unregistered_poller.sh
		fi
	fi

	if [[ $SPDK_TEST_RAID -eq 1 ]]; then
		run_test "bdev_raid" $rootdir/test/bdev/bdev_raid.sh
		run_test "spdkcli_raid" $rootdir/test/spdkcli/raid.sh
		run_test "blockdev_raid5f" $rootdir/test/bdev/blockdev.sh "raid5f"
	fi

	if [[ $(uname -s) == Linux ]]; then
		if [[ $SPDK_TEST_BLOCKDEV -eq 1 || $SPDK_TEST_URING -eq 1 ]]; then
			# The crypto job also includes the SPDK_TEST_BLOCKDEV in its configuration hence the
			# dd tests are executed there as well. However, these tests can take a significant
			# amount of time to complete (up to 4min) on a physical system leading to a potential
			# job timeout. Avoid that by skipping these tests - this should not affect the coverage
			# since dd tests are still run as part of the vg jobs.
			if [[ $SPDK_TEST_CRYPTO -eq 0 ]]; then
				run_test "spdk_dd" $rootdir/test/dd/dd.sh
			fi
		fi
	fi

	if [ $SPDK_TEST_NVME -eq 1 ]; then
		run_test "blockdev_nvme" $rootdir/test/bdev/blockdev.sh "nvme"
		if [[ $(uname -s) == Linux ]]; then
			run_test "blockdev_nvme_gpt" $rootdir/test/bdev/blockdev.sh "gpt"
		fi
		run_test "nvme" $rootdir/test/nvme/nvme.sh
		if [[ $SPDK_TEST_NVME_PMR -eq 1 ]]; then
			run_test "nvme_pmr" $rootdir/test/nvme/nvme_pmr.sh
		fi

		run_test "nvme_scc" $rootdir/test/nvme/nvme_scc.sh

		if [[ $SPDK_TEST_NVME_BP -eq 1 ]]; then
			run_test "nvme_bp" $rootdir/test/nvme/nvme_bp.sh
		fi
		if [[ $SPDK_TEST_NVME_CUSE -eq 1 ]]; then
			run_test "nvme_cuse" $rootdir/test/nvme/cuse/nvme_cuse.sh
		fi
		if [[ $SPDK_TEST_NVME_CMB -eq 1 ]]; then
			run_test "nvme_cmb" $rootdir/test/nvme/cmb/cmb.sh
		fi
		if [[ $SPDK_TEST_NVME_FDP -eq 1 ]]; then
			run_test "nvme_fdp" test/nvme/nvme_fdp.sh
		fi

		if [[ $SPDK_TEST_NVME_ZNS -eq 1 ]]; then
			run_test "nvme_zns" $rootdir/test/nvme/zns/zns.sh
		fi

		run_test "nvme_rpc" $rootdir/test/nvme/nvme_rpc.sh
		run_test "nvme_rpc_timeouts" $rootdir/test/nvme/nvme_rpc_timeouts.sh

		if [ $(uname -s) = Linux ]; then
			run_test "sw_hotplug" $rootdir/test/nvme/sw_hotplug.sh
		fi

		if [[ $SPDK_TEST_XNVME -eq 1 ]]; then
			run_test "nvme_xnvme" $rootdir/test/nvme/xnvme/xnvme.sh
			run_test "blockdev_xnvme" $rootdir/test/bdev/blockdev.sh "xnvme"
			# Run ublk with xnvme since they have similar kernel dependencies
			run_test "ublk" $rootdir/test/ublk/ublk.sh
			run_test "ublk_recovery" $rootdir/test/ublk/ublk_recovery.sh
		fi

		if [[ $SPDK_TEST_NVME_INTERRUPT -eq 1 ]]; then
			run_test "nvme_interrupt" "$rootdir/test/nvme/interrupt.sh"
		fi
	fi

	if [ $SPDK_TEST_IOAT -eq 1 ]; then
		run_test "ioat" $rootdir/test/ioat/ioat.sh
	fi

	timing_exit lib

	if [ $SPDK_TEST_ISCSI -eq 1 ]; then
		run_test "iscsi_tgt" $rootdir/test/iscsi_tgt/iscsi_tgt.sh
		run_test "spdkcli_iscsi" $rootdir/test/spdkcli/iscsi.sh
	fi

	if [ $SPDK_TEST_NVMF -eq 1 ]; then
		export NET_TYPE
		# The NVMe-oF run test cases are split out like this so that the parser that compiles the
		# list of all tests can properly differentiate them. Please do not merge them into one line.
		if [ "$SPDK_TEST_NVMF_TRANSPORT" = "rdma" ]; then
			run_test "nvmf_rdma" $rootdir/test/nvmf/nvmf.sh --transport=$SPDK_TEST_NVMF_TRANSPORT
			run_test "spdkcli_nvmf_rdma" $rootdir/test/spdkcli/nvmf.sh --transport=$SPDK_TEST_NVMF_TRANSPORT
		elif [ "$SPDK_TEST_NVMF_TRANSPORT" = "tcp" ]; then
			run_test "nvmf_tcp" $rootdir/test/nvmf/nvmf.sh --transport=$SPDK_TEST_NVMF_TRANSPORT
			if [[ $SPDK_TEST_URING -eq 0 ]]; then
				run_test "spdkcli_nvmf_tcp" $rootdir/test/spdkcli/nvmf.sh --transport=$SPDK_TEST_NVMF_TRANSPORT
				run_test "nvmf_identify_passthru" $rootdir/test/nvmf/target/identify_passthru.sh --transport=$SPDK_TEST_NVMF_TRANSPORT
			fi
			run_test "nvmf_dif" $rootdir/test/nvmf/target/dif.sh
			run_test "nvmf_abort_qd_sizes" $rootdir/test/nvmf/target/abort_qd_sizes.sh
			# The keyring tests utilize NVMe/TLS
			run_test "keyring_file" "$rootdir/test/keyring/file.sh"
			if [[ "$CONFIG_HAVE_KEYUTILS" == y ]]; then
				run_test "keyring_linux" "$rootdir/scripts/keyctl-session-wrapper" \
					"$rootdir/test/keyring/linux.sh"
			fi
		elif [ "$SPDK_TEST_NVMF_TRANSPORT" = "fc" ]; then
			run_test "nvmf_fc" $rootdir/test/nvmf/nvmf.sh --transport=$SPDK_TEST_NVMF_TRANSPORT
			run_test "spdkcli_nvmf_fc" $rootdir/test/spdkcli/nvmf.sh
		else
			echo "unknown NVMe transport, please specify rdma, tcp, or fc."
			exit 1
		fi
	fi

	# For vfio_user and vhost tests We need to make sure entire HUGEMEM default
	# goes to a single node as we share hugepages with qemu instance(s) and we
	# cannot split it across all numa nodes without making sure there's enough
	# memory available.

	if [ $SPDK_TEST_VHOST -eq 1 ]; then
		HUGENODE=0 run_test "vhost" $rootdir/test/vhost/vhost.sh --iso
	fi

	if [ $SPDK_TEST_VFIOUSER_QEMU -eq 1 ]; then
		HUGENODE=0 run_test "vfio_user_qemu" $rootdir/test/vfio_user/vfio_user.sh --iso
	fi

	if [ $SPDK_TEST_LVOL -eq 1 ]; then
		run_test "blobstore" $rootdir/test/blobstore/blobstore.sh
		run_test "blobstore_grow" $rootdir/test/blobstore/blobstore_grow/blobstore_grow.sh
		run_test "max_growable" $rootdir/test/blobstore/blobstore_grow/max_growable.sh
		run_test "hello_blob" $SPDK_EXAMPLE_DIR/hello_blob \
			examples/blob/hello_world/hello_blob.json
		run_test "lvol" $rootdir/test/lvol/lvol.sh
		run_test "blob_io_wait" $rootdir/test/blobstore/blob_io_wait/blob_io_wait.sh
	fi

	if [ $SPDK_TEST_VHOST_INIT -eq 1 ]; then
		timing_enter vhost_initiator
		run_test "vhost_blockdev" $rootdir/test/vhost/initiator/blockdev.sh
		run_test "spdkcli_virtio" $rootdir/test/spdkcli/virtio.sh
		run_test "vhost_shared" $rootdir/test/vhost/shared/shared.sh
		run_test "vhost_fuzz" $rootdir/test/vhost/fuzz/fuzz.sh
		timing_exit vhost_initiator
	fi

	if [ $SPDK_TEST_RBD -eq 1 ]; then
		run_test "blockdev_rbd" $rootdir/test/bdev/blockdev.sh "rbd"
		run_test "spdkcli_rbd" $rootdir/test/spdkcli/rbd.sh
	fi

	if [ $SPDK_TEST_OCF -eq 1 ]; then
		run_test "ocf" $rootdir/test/ocf/ocf.sh
	fi

	if [ $SPDK_TEST_FTL -eq 1 ]; then
		run_test "ftl" $rootdir/test/ftl/ftl.sh
	fi

	if [ $SPDK_TEST_VMD -eq 1 ]; then
		run_test "vmd" $rootdir/test/vmd/vmd.sh
	fi

	if [ $SPDK_TEST_OPAL -eq 1 ]; then
		run_test "nvme_opal" $rootdir/test/nvme/nvme_opal.sh
	fi

	if [ $SPDK_TEST_CRYPTO -eq 1 ]; then
		run_test "blockdev_crypto_aesni" $rootdir/test/bdev/blockdev.sh "crypto_aesni"
		run_test "blockdev_crypto_sw" $rootdir/test/bdev/blockdev.sh "crypto_sw"
		run_test "blockdev_crypto_qat" $rootdir/test/bdev/blockdev.sh "crypto_qat"
		run_test "chaining" $rootdir/test/bdev/chaining.sh
	fi

	if [[ $SPDK_TEST_SCHEDULER -eq 1 ]]; then
		run_test "scheduler" $rootdir/test/scheduler/scheduler.sh
	fi

	if [[ $SPDK_TEST_SMA -eq 1 ]]; then
		run_test "sma" $rootdir/test/sma/sma.sh
	fi

	if [[ $SPDK_TEST_FUZZER -eq 1 ]]; then
		run_test "llvm_fuzz" $rootdir/test/fuzz/llvm.sh
	fi

	if [[ $SPDK_TEST_ACCEL_MLX5 -eq 1 ]]; then
		run_test "accel_mlx5_crypto" $rootdir/test/accel/mlx5/accel_mlx5_crypto.sh
		run_test "accel_mlx5_copy" $rootdir/test/accel/mlx5/accel_mlx5_copy.sh
		run_test "accel_mlx5_crc32c" $rootdir/test/accel/mlx5/accel_mlx5_crc32c.sh
	fi
fi

trap - SIGINT SIGTERM EXIT

timing_enter post_cleanup
autotest_cleanup
timing_exit post_cleanup

timing_exit autotest
chmod a+r $output_dir/timing.txt

[[ -f "$output_dir/udev.log" ]] && rm -f "$output_dir/udev.log"

gather_coverage
