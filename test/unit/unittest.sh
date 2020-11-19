#!/usr/bin/env bash
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
	$valgrind $testdir/lib/bdev/bdev_ocssd.c/bdev_ocssd_ut
	$valgrind $testdir/lib/bdev/raid/bdev_raid.c/bdev_raid_ut
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
	$valgrind $testdir/lib/blobfs/tree.c/tree_ut
	$valgrind $testdir/lib/blobfs/blobfs_async_ut/blobfs_async_ut
	# blobfs_sync_ut hangs when run under valgrind, so don't use $valgrind
	$testdir/lib/blobfs/blobfs_sync_ut/blobfs_sync_ut
	$valgrind $testdir/lib/blobfs/blobfs_bdev.c/blobfs_bdev_ut
}

function unittest_event() {
	$valgrind $testdir/lib/event/subsystem.c/subsystem_ut
	$valgrind $testdir/lib/event/app.c/app_ut
	$valgrind $testdir/lib/event/reactor.c/reactor_ut
}

function unittest_ftl() {
	$valgrind $testdir/lib/ftl/ftl_ppa/ftl_ppa_ut
	$valgrind $testdir/lib/ftl/ftl_band.c/ftl_band_ut
	$valgrind $testdir/lib/ftl/ftl_reloc.c/ftl_reloc_ut
	$valgrind $testdir/lib/ftl/ftl_wptr/ftl_wptr_ut
	$valgrind $testdir/lib/ftl/ftl_md/ftl_md_ut
	$valgrind $testdir/lib/ftl/ftl_io.c/ftl_io_ut
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
	$valgrind $testdir/lib/nvme/nvme_uevent.c/nvme_uevent_ut
}

function unittest_nvmf() {
	$valgrind $testdir/lib/nvmf/ctrlr.c/ctrlr_ut
	$valgrind $testdir/lib/nvmf/ctrlr_bdev.c/ctrlr_bdev_ut
	$valgrind $testdir/lib/nvmf/ctrlr_discovery.c/ctrlr_discovery_ut
	$valgrind $testdir/lib/nvmf/subsystem.c/subsystem_ut
	$valgrind $testdir/lib/nvmf/tcp.c/tcp_ut
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
	if grep -q '#define SPDK_CONFIG_URING 1' $rootdir/include/spdk/config.h; then
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
	$valgrind $testdir/lib/util/string.c/string_ut
	$valgrind $testdir/lib/util/dif.c/dif_ut
	$valgrind $testdir/lib/util/iov.c/iov_ut
	$valgrind $testdir/lib/util/math.c/math_ut
	$valgrind $testdir/lib/util/pipe.c/pipe_ut
}

# if ASAN is enabled, use it.  If not use valgrind if installed but allow
# the env variable to override the default shown below.
if [ -z ${valgrind+x} ]; then
	if grep -q '#undef SPDK_CONFIG_ASAN' $rootdir/include/spdk/config.h && hash valgrind; then
		valgrind='valgrind --leak-check=full --error-exitcode=2'
	else
		valgrind=''
	fi
fi

# setup local unit test coverage if cov is available
if hash lcov && grep -q '#define SPDK_CONFIG_COVERAGE 1' $rootdir/include/spdk/config.h; then
	cov_avail="yes"
else
	cov_avail="no"
fi
if [ "$cov_avail" = "yes" ]; then
	# set unit test output dir if not specified in env var
	if [ -z ${UT_COVERAGE+x} ]; then
		UT_COVERAGE="ut_coverage"
	fi
	mkdir -p $UT_COVERAGE
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
	$LCOV -q -c -i -d . -t "Baseline" -o $UT_COVERAGE/ut_cov_base.info
fi

# workaround for valgrind v3.13 on arm64
if [ $(uname -m) = "aarch64" ]; then
	export LD_HWCAP_MASK=1
fi

run_test "unittest_include" $valgrind $testdir/include/spdk/histogram_data.h/histogram_ut
run_test "unittest_bdev" unittest_bdev
if grep -q '#define SPDK_CONFIG_CRYPTO 1' $rootdir/include/spdk/config.h; then
	run_test "unittest_bdev_crypto" $valgrind $testdir/lib/bdev/crypto.c/crypto_ut
fi

if grep -q '#define SPDK_CONFIG_REDUCE 1' $rootdir/include/spdk/config.h; then
	run_test "unittest_bdev_reduce" $valgrind $testdir/lib/bdev/compress.c/compress_ut
fi

if grep -q '#define SPDK_CONFIG_PMDK 1' $rootdir/include/spdk/config.h; then
	run_test "unittest_bdev_pmem" $valgrind $testdir/lib/bdev/pmem/bdev_pmem_ut
fi

if grep -q '#define SPDK_CONFIG_RAID5 1' $rootdir/include/spdk/config.h; then
	run_test "unittest_bdev_raid5" $valgrind $testdir/lib/bdev/raid/raid5.c/raid5_ut
fi

run_test "unittest_blob_blobfs" unittest_blob
run_test "unittest_event" unittest_event
if [ $(uname -s) = Linux ]; then
	run_test "unittest_ftl" unittest_ftl
fi

run_test "unittest_ioat" $valgrind $testdir/lib/ioat/ioat.c/ioat_ut
if grep -q '#define SPDK_CONFIG_IDXD 1' $rootdir/include/spdk/config.h; then
	run_test "unittest_idxd" $valgrind $testdir/lib/idxd/idxd.c/idxd_ut
fi
run_test "unittest_iscsi" unittest_iscsi
run_test "unittest_json" unittest_json
run_test "unittest_notify" $valgrind $testdir/lib/notify/notify.c/notify_ut
run_test "unittest_nvme" unittest_nvme
run_test "unittest_log" $valgrind $testdir/lib/log/log.c/log_ut
run_test "unittest_lvol" $valgrind $testdir/lib/lvol/lvol.c/lvol_ut
if grep -q '#define SPDK_CONFIG_RDMA 1' $rootdir/include/spdk/config.h; then
	run_test "unittest_nvme_rdma" $valgrind $testdir/lib/nvme/nvme_rdma.c/nvme_rdma_ut
fi

run_test "unittest_nvmf" unittest_nvmf
if grep -q '#define SPDK_CONFIG_FC 1' $rootdir/include/spdk/config.h; then
	run_test "unittest_nvmf_fc" $valgrind $testdir/lib/nvmf/fc.c/fc_ut
	run_test "unittest_nvmf_fc_ls" $valgrind $testdir/lib/nvmf/fc_ls.c/fc_ls_ut
fi

if grep -q '#define SPDK_CONFIG_RDMA 1' $rootdir/include/spdk/config.h; then
	run_test "unittest_nvmf_rdma" $valgrind $testdir/lib/nvmf/rdma.c/rdma_ut
fi

run_test "unittest_scsi" unittest_scsi
run_test "unittest_sock" unittest_sock
run_test "unittest_thread" $valgrind $testdir/lib/thread/thread.c/thread_ut
run_test "unittest_util" unittest_util
if grep -q '#define SPDK_CONFIG_VHOST 1' $rootdir/include/spdk/config.h; then
	run_test "unittest_vhost" $valgrind $testdir/lib/vhost/vhost.c/vhost_ut
fi

# local unit test coverage
# lcov takes considerable time to process clang coverage.
# Disabling lcov allow us to do this.
# More information: https://github.com/spdk/spdk/issues/1693
CC_TYPE=$(grep CC_TYPE mk/cc.mk)
if [ "$cov_avail" = "yes" ] && ! [[ "$CC_TYPE" == *"clang"* ]]; then
	$LCOV -q -d . -c -t "$(hostname)" -o $UT_COVERAGE/ut_cov_test.info
	$LCOV -q -a $UT_COVERAGE/ut_cov_base.info -a $UT_COVERAGE/ut_cov_test.info -o $UT_COVERAGE/ut_cov_total.info
	$LCOV -q -a $UT_COVERAGE/ut_cov_total.info -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info "$rootdir/app/*" -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info "$rootdir/dpdk/*" -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info "$rootdir/examples/*" -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info "$rootdir/include/*" -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info "$rootdir/lib/vhost/rte_vhost/*" -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info "$rootdir/test/*" -o $UT_COVERAGE/ut_cov_unit.info
	rm -f $UT_COVERAGE/ut_cov_base.info $UT_COVERAGE/ut_cov_test.info
	genhtml $UT_COVERAGE/ut_cov_unit.info --output-directory $UT_COVERAGE
	# git -C option not used for compatibility reasons
	(cd $rootdir && git clean -f "*.gcda")
fi

set +x

echo
echo
echo "====================="
echo "All unit tests passed"
echo "====================="
if [ "$cov_avail" = "yes" ]; then
	echo "Note: coverage report is here: $rootdir/$UT_COVERAGE"
else
	echo "WARN: lcov not installed or SPDK built without coverage!"
fi
if grep -q '#undef SPDK_CONFIG_ASAN' $rootdir/include/spdk/config.h && [ "$valgrind" = "" ]; then
	echo "WARN: neither valgrind nor ASAN is enabled!"
fi

echo
echo
