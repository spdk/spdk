#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
# Environment variables:
#  $valgrind    Specify the valgrind command line, if not
#               then a default command line is used

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $(dirname $0)/../..)
source "$rootdir/test/common/autotest_common.sh"

cd "$rootdir"

function unittest_bdev() {
	$valgrind $testdir/lib/bdev/bdev.c/bdev_ut
	$valgrind $testdir/lib/bdev/nvme/bdev_nvme.c/bdev_nvme_ut
	$valgrind $testdir/lib/bdev/raid/bdev_raid.c/bdev_raid_ut
	$valgrind $testdir/lib/bdev/raid/bdev_raid_sb.c/bdev_raid_sb_ut
	$valgrind $testdir/lib/bdev/raid/concat.c/concat_ut
	$valgrind $testdir/lib/bdev/raid/raid0.c/raid0_ut
	$valgrind $testdir/lib/bdev/raid/raid1.c/raid1_ut
	$valgrind $testdir/lib/bdev/bdev_zone.c/bdev_zone_ut
	$valgrind $testdir/lib/bdev/gpt/gpt.c/gpt_ut
	$valgrind $testdir/lib/bdev/part.c/part_ut
	$valgrind $testdir/lib/bdev/scsi_nvme.c/scsi_nvme_ut
	$valgrind $testdir/lib/bdev/vbdev_lvol.c/vbdev_lvol_ut
	$valgrind $testdir/lib/bdev/vbdev_zone_block.c/vbdev_zone_block_ut
	$valgrind $testdir/lib/bdev/mt/bdev.c/bdev_ut
}

function unittest_blob() {
	# We do not compile blob_ut on systems with too old Cunit, so do
	# not try to execute it if it doesn't exist
	if [[ -e $testdir/lib/blob/blob.c/blob_ut ]]; then
		$valgrind $testdir/lib/blob/blob.c/blob_ut
	fi
	$valgrind $testdir/lib/blob/blob_bdev.c/blob_bdev_ut
}

function unittest_event() {
	$valgrind $testdir/lib/event/app.c/app_ut
	$valgrind $testdir/lib/event/reactor.c/reactor_ut
}

function unittest_ftl() {
	$valgrind $testdir/lib/ftl/ftl_band.c/ftl_band_ut
	$valgrind $testdir/lib/ftl/ftl_bitmap.c/ftl_bitmap_ut
	$valgrind $testdir/lib/ftl/ftl_io.c/ftl_io_ut
	$valgrind $testdir/lib/ftl/ftl_mngt/ftl_mngt_ut
	$valgrind $testdir/lib/ftl/ftl_mempool.c/ftl_mempool_ut
	$valgrind $testdir/lib/ftl/ftl_l2p/ftl_l2p_ut
	$valgrind $testdir/lib/ftl/ftl_sb/ftl_sb_ut
	$valgrind $testdir/lib/ftl/ftl_layout_upgrade/ftl_layout_upgrade_ut
	$valgrind $testdir/lib/ftl/ftl_p2l.c/ftl_p2l_ut
}

function unittest_iscsi() {
	$valgrind $testdir/lib/iscsi/conn.c/conn_ut
	$valgrind $testdir/lib/iscsi/param.c/param_ut
	$valgrind $testdir/lib/iscsi/tgt_node.c/tgt_node_ut
	$valgrind $testdir/lib/iscsi/iscsi.c/iscsi_ut
	$valgrind $testdir/lib/iscsi/init_grp.c/init_grp_ut
	$valgrind $testdir/lib/iscsi/portal_grp.c/portal_grp_ut
}

function unittest_json() {
	$valgrind $testdir/lib/json/json_parse.c/json_parse_ut
	$valgrind $testdir/lib/json/json_util.c/json_util_ut
	$valgrind $testdir/lib/json/json_write.c/json_write_ut
	$valgrind $testdir/lib/jsonrpc/jsonrpc_server.c/jsonrpc_server_ut
}

function unittest_rpc() {
	$valgrind $testdir/lib/rpc/rpc.c/rpc_ut
}

function unittest_nvme() {
	$valgrind $testdir/lib/nvme/nvme.c/nvme_ut
	$valgrind $testdir/lib/nvme/nvme_ctrlr.c/nvme_ctrlr_ut
	$valgrind $testdir/lib/nvme/nvme_ctrlr_cmd.c/nvme_ctrlr_cmd_ut
	$valgrind $testdir/lib/nvme/nvme_ctrlr_ocssd_cmd.c/nvme_ctrlr_ocssd_cmd_ut
	$valgrind $testdir/lib/nvme/nvme_ns.c/nvme_ns_ut
	$valgrind $testdir/lib/nvme/nvme_ns_cmd.c/nvme_ns_cmd_ut
	$valgrind $testdir/lib/nvme/nvme_ns_ocssd_cmd.c/nvme_ns_ocssd_cmd_ut
	$valgrind $testdir/lib/nvme/nvme_qpair.c/nvme_qpair_ut
	$valgrind $testdir/lib/nvme/nvme_pcie.c/nvme_pcie_ut
	$valgrind $testdir/lib/nvme/nvme_poll_group.c/nvme_poll_group_ut
	$valgrind $testdir/lib/nvme/nvme_quirks.c/nvme_quirks_ut
	$valgrind $testdir/lib/nvme/nvme_tcp.c/nvme_tcp_ut
	$valgrind $testdir/lib/nvme/nvme_transport.c/nvme_transport_ut
	$valgrind $testdir/lib/nvme/nvme_io_msg.c/nvme_io_msg_ut
	$valgrind $testdir/lib/nvme/nvme_pcie_common.c/nvme_pcie_common_ut
	$valgrind $testdir/lib/nvme/nvme_fabric.c/nvme_fabric_ut
	$valgrind $testdir/lib/nvme/nvme_opal.c/nvme_opal_ut
}

function unittest_nvmf() {
	$valgrind $testdir/lib/nvmf/ctrlr.c/ctrlr_ut
	$valgrind $testdir/lib/nvmf/ctrlr_bdev.c/ctrlr_bdev_ut
	$valgrind $testdir/lib/nvmf/ctrlr_discovery.c/ctrlr_discovery_ut
	$valgrind $testdir/lib/nvmf/subsystem.c/subsystem_ut
	$valgrind $testdir/lib/nvmf/tcp.c/tcp_ut
	$valgrind $testdir/lib/nvmf/nvmf.c/nvmf_ut
}

function unittest_scsi() {
	$valgrind $testdir/lib/scsi/dev.c/dev_ut
	$valgrind $testdir/lib/scsi/lun.c/lun_ut
	$valgrind $testdir/lib/scsi/scsi.c/scsi_ut
	$valgrind $testdir/lib/scsi/scsi_bdev.c/scsi_bdev_ut
	$valgrind $testdir/lib/scsi/scsi_pr.c/scsi_pr_ut
}

function unittest_sock() {
	$valgrind $testdir/lib/sock/sock.c/sock_ut
	$valgrind $testdir/lib/sock/posix.c/posix_ut
	# Check whether uring is configured
	if [[ $CONFIG_URING == y ]]; then
		$valgrind $testdir/lib/sock/uring.c/uring_ut
	fi
}

function unittest_util() {
	$valgrind $testdir/lib/util/base64.c/base64_ut
	$valgrind $testdir/lib/util/bit_array.c/bit_array_ut
	$valgrind $testdir/lib/util/cpuset.c/cpuset_ut
	$valgrind $testdir/lib/util/crc16.c/crc16_ut
	$valgrind $testdir/lib/util/crc32_ieee.c/crc32_ieee_ut
	$valgrind $testdir/lib/util/crc32c.c/crc32c_ut
	$valgrind $testdir/lib/util/crc64.c/crc64_ut
	$valgrind $testdir/lib/util/string.c/string_ut
	$valgrind $testdir/lib/util/dif.c/dif_ut
	$valgrind $testdir/lib/util/iov.c/iov_ut
	$valgrind $testdir/lib/util/math.c/math_ut
	$valgrind $testdir/lib/util/pipe.c/pipe_ut
	if [ $(uname -s) = Linux ]; then
		$valgrind $testdir/lib/util/fd_group.c/fd_group_ut
	fi
}

function unittest_fsdev() {
	$valgrind $testdir/lib/fsdev/fsdev.c/fsdev_ut
}

function unittest_init() {
	$valgrind $testdir/lib/init/subsystem.c/subsystem_ut
}

if [ $SPDK_RUN_VALGRIND -eq 1 ] && [ $SPDK_RUN_ASAN -eq 1 ]; then
	echo "ERR: Tests cannot be run if both SPDK_RUN_VALGRIND and SPDK_RUN_ASAN options are selected simultaneously"
	exit 1
fi

# if ASAN is enabled, use it.  If not use valgrind if installed but allow
# the env variable to override the default shown below.
if [ -z ${valgrind+x} ]; then
	if [[ $CONFIG_ASAN == n ]] && hash valgrind; then
		valgrind='valgrind --leak-check=full --error-exitcode=2 --verbose'
	else
		valgrind=''
	fi
fi
if [ $SPDK_RUN_VALGRIND -eq 1 ]; then
	if [ -n "$valgrind" ]; then
		run_test "valgrind" echo "Using valgrind for unit tests"
	else
		echo "ERR: SPDK_RUN_VALGRIND option is enabled but valgrind is not available"
		exit 1
	fi
fi

if [[ $CONFIG_COVERAGE == y ]]; then
	UT_COVERAGE=$output_dir/ut_coverage
	mkdir -p "$UT_COVERAGE"
	# zero out coverage data
	$LCOV -q -c --no-external -i -d . -t "Baseline" -o $UT_COVERAGE/ut_cov_base.info
fi

# workaround for valgrind v3.13 on arm64
if [ $(uname -m) = "aarch64" ]; then
	export LD_HWCAP_MASK=1
fi

run_test "unittest_pci_event" $valgrind $testdir/lib/env_dpdk/pci_event.c/pci_event_ut
run_test "unittest_include" $valgrind $testdir/include/spdk/histogram_data.h/histogram_ut
run_test "unittest_bdev" unittest_bdev
if [[ $CONFIG_CRYPTO == y ]]; then
	run_test "unittest_bdev_crypto" $valgrind $testdir/lib/bdev/crypto.c/crypto_ut
	run_test "unittest_bdev_crypto" $valgrind $testdir/lib/accel/dpdk_cryptodev.c/accel_dpdk_cryptodev_ut
fi

if [[ $CONFIG_DPDK_COMPRESSDEV == y ]]; then
	run_test "unittest_dpdk_compressdev" $valgrind $testdir/lib/accel/dpdk_compressdev.c/accel_dpdk_compressdev_ut
fi

if [[ $CONFIG_RAID5F == y ]]; then
	run_test "unittest_bdev_raid5f" $valgrind $testdir/lib/bdev/raid/raid5f.c/raid5f_ut
fi

run_test "unittest_blob" unittest_blob
run_test "unittest_event" unittest_event
if [ $(uname -s) = Linux ]; then
	run_test "unittest_ftl" unittest_ftl
fi

run_test "unittest_accel" $valgrind $testdir/lib/accel/accel.c/accel_ut
run_test "unittest_ioat" $valgrind $testdir/lib/ioat/ioat.c/ioat_ut
if [[ $CONFIG_IDXD == y ]]; then
	run_test "unittest_idxd_user" $valgrind $testdir/lib/idxd/idxd_user.c/idxd_user_ut
fi
run_test "unittest_iscsi" unittest_iscsi
run_test "unittest_json" unittest_json
run_test "unittest_rpc" unittest_rpc
run_test "unittest_notify" $valgrind $testdir/lib/notify/notify.c/notify_ut
run_test "unittest_nvme" unittest_nvme
run_test "unittest_log" $valgrind $testdir/lib/log/log.c/log_ut
run_test "unittest_lvol" $valgrind $testdir/lib/lvol/lvol.c/lvol_ut
if [[ $CONFIG_RDMA == y ]]; then
	run_test "unittest_nvme_rdma" $valgrind $testdir/lib/nvme/nvme_rdma.c/nvme_rdma_ut
	run_test "unittest_nvmf_transport" $valgrind $testdir/lib/nvmf/transport.c/transport_ut
	run_test "unittest_rdma" $valgrind $testdir/lib/rdma/common.c/common_ut
	run_test "unittest_nvmf_rdma" $valgrind $testdir/lib/nvmf/rdma.c/rdma_ut
fi

if [[ $CONFIG_NVME_CUSE == y ]]; then
	run_test "unittest_nvme_cuse" $valgrind $testdir/lib/nvme/nvme_cuse.c/nvme_cuse_ut
fi

run_test "unittest_nvmf" unittest_nvmf
if [[ $CONFIG_FC == y ]]; then
	run_test "unittest_nvmf_fc" $valgrind $testdir/lib/nvmf/fc.c/fc_ut
	run_test "unittest_nvmf_fc_ls" $valgrind $testdir/lib/nvmf/fc_ls.c/fc_ls_ut
fi

if [[ $CONFIG_VFIO_USER == y ]]; then
	run_test "unittest_nvmf_vfio_user" $valgrind $testdir/lib/nvmf/vfio_user.c/vfio_user_ut
fi

run_test "unittest_scsi" unittest_scsi
if [ $(uname -s) = Linux ]; then
	# There are several intermittent sock_ut failures on FreeBSD that need to be debugged.
	# So just disable running it on FreeBSD for now.  See issue #2943.
	run_test "unittest_sock" unittest_sock
fi
run_test "unittest_thread" $valgrind $testdir/lib/thread/thread.c/thread_ut
run_test "unittest_iobuf" $valgrind $testdir/lib/thread/iobuf.c/iobuf_ut
run_test "unittest_util" unittest_util
if [[ $CONFIG_FSDEV == y ]]; then
	run_test "unittest_fsdev" unittest_fsdev
fi
if [[ $CONFIG_VHOST == y ]]; then
	run_test "unittest_vhost" $valgrind $testdir/lib/vhost/vhost.c/vhost_ut
fi
run_test "unittest_dma" $valgrind $testdir/lib/dma/dma.c/dma_ut

run_test "unittest_init" unittest_init
run_test "unittest_keyring" $valgrind "$testdir/lib/keyring/keyring.c/keyring_ut"

if [[ $CONFIG_COVERAGE == y ]]; then
	$LCOV -q -d . -c --no-external -t "$(hostname)" -o $UT_COVERAGE/ut_cov_test.info
	$LCOV -q -a $UT_COVERAGE/ut_cov_base.info -a $UT_COVERAGE/ut_cov_test.info -o $UT_COVERAGE/ut_cov_total.info
	$LCOV -q -a $UT_COVERAGE/ut_cov_total.info -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info "$rootdir/app/*" -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info "$rootdir/dpdk/*" -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info "$rootdir/examples/*" -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info "$rootdir/test/*" -o $UT_COVERAGE/ut_cov_unit.info
	rm -f $UT_COVERAGE/ut_cov_base.info $UT_COVERAGE/ut_cov_test.info
	genhtml $UT_COVERAGE/ut_cov_unit.info --output-directory $UT_COVERAGE
	echo "Note: coverage report is here: $UT_COVERAGE"
fi

set +x

echo
echo
echo "====================="
echo "All unit tests passed"
echo "====================="
if [[ $CONFIG_ASAN == n && $valgrind = "" ]]; then
	echo "WARN: neither valgrind nor ASAN is enabled!"
fi

echo
echo
