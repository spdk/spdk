#!/usr/bin/env bash
#
# Environment variables:
#  $valgrind    Specify the valgrind command line, if not
#               then a default command line is used

set -xe

rootdir=$(readlink -f $(dirname $0))

# if ASAN is enabled, use it.  If not use valgrind if installed but allow
# the env variable to override the default shown below.
if [ -z ${valgrind+x} ]; then
	if grep -q '#undef SPDK_CONFIG_ASAN' config.h && hash valgrind; then
		valgrind='valgrind --leak-check=full --error-exitcode=2'
	else
		valgrind=''
	fi
fi

# setup local unit test coverage if cov is available
if hash lcov && grep -q '#define SPDK_CONFIG_COVERAGE 1' config.h; then
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
echo `pwd`
$valgrind $rootdir/include/spdk/histogram_data.h/histogram_ut

$valgrind $rootdir/lib/bdev/bdev.c/bdev_ut
$valgrind $rootdir/lib/bdev/part.c/part_ut
$valgrind $rootdir/lib/bdev/scsi_nvme.c/scsi_nvme_ut
$valgrind $rootdir/lib/bdev/gpt/gpt.c/gpt_ut
$valgrind $rootdir/lib/bdev/vbdev_lvol.c/vbdev_lvol_ut

if grep -q '#define SPDK_CONFIG_NVML 1' config.h; then
	$valgrind $rootdir/lib/bdev/pmem/bdev_pmem_ut
fi

$valgrind $rootdir/lib/bdev/mt/bdev.c/bdev_ut

$valgrind $rootdir/lib/blob/blob.c/blob_ut
$valgrind $rootdir/lib/blobfs/tree.c/tree_ut

$valgrind $rootdir/lib/blobfs/blobfs_async_ut/blobfs_async_ut
# blobfs_sync_ut hangs when run under valgrind, so don't use $valgrind
$rootdir/lib/blobfs/blobfs_sync_ut/blobfs_sync_ut

$valgrind $rootdir/lib/event/subsystem.c/subsystem_ut

$valgrind $rootdir/lib/net/sock.c/sock_ut

$valgrind $rootdir/lib/nvme/nvme.c/nvme_ut
$valgrind $rootdir/lib/nvme/nvme_ctrlr.c/nvme_ctrlr_ut
$valgrind $rootdir/lib/nvme/nvme_ctrlr_cmd.c/nvme_ctrlr_cmd_ut
$valgrind $rootdir/lib/nvme/nvme_ns.c/nvme_ns_ut
$valgrind $rootdir/lib/nvme/nvme_ns_cmd.c/nvme_ns_cmd_ut
$valgrind $rootdir/lib/nvme/nvme_qpair.c/nvme_qpair_ut
$valgrind $rootdir/lib/nvme/nvme_pcie.c/nvme_pcie_ut
$valgrind $rootdir/lib/nvme/nvme_quirks.c/nvme_quirks_ut

$valgrind $rootdir/lib/ioat/ioat.c/ioat_ut

$valgrind $rootdir/lib/json/json_parse.c/json_parse_ut
$valgrind $rootdir/lib/json/json_util.c/json_util_ut
$valgrind $rootdir/lib/json/json_write.c/json_write_ut

$valgrind $rootdir/lib/jsonrpc/jsonrpc_server.c/jsonrpc_server_ut

$valgrind $rootdir/lib/log/log.c/log_ut

$valgrind $rootdir/lib/nvmf/ctrlr.c/ctrlr_ut
$valgrind $rootdir/lib/nvmf/ctrlr_bdev.c/ctrlr_bdev_ut
$valgrind $rootdir/lib/nvmf/ctrlr_discovery.c/ctrlr_discovery_ut
$valgrind $rootdir/lib/nvmf/request.c/request_ut
$valgrind $rootdir/lib/nvmf/subsystem.c/subsystem_ut

$valgrind $rootdir/lib/scsi/dev.c/dev_ut
$valgrind $rootdir/lib/scsi/lun.c/lun_ut
$valgrind $rootdir/lib/scsi/scsi.c/scsi_ut
$valgrind $rootdir/lib/scsi/scsi_bdev.c/scsi_bdev_ut

$valgrind $rootdir/lib/lvol/lvol.c/lvol_ut

$valgrind $rootdir/lib/iscsi/conn.c/conn_ut
$valgrind $rootdir/lib/iscsi/param.c/param_ut
$valgrind $rootdir/lib/iscsi/tgt_node.c/tgt_node_ut $rootdir/lib/iscsi/tgt_node.c/tgt_node.conf
$valgrind $rootdir/lib/iscsi/iscsi.c/iscsi_ut
$valgrind $rootdir/lib/iscsi/init_grp.c/init_grp_ut $rootdir/lib/iscsi/init_grp.c/init_grp.conf
$valgrind $rootdir/lib/iscsi/portal_grp.c/portal_grp_ut $rootdir/lib/iscsi/portal_grp.c/portal_grp.conf

$valgrind $rootdir/lib/util/bit_array.c/bit_array_ut
$valgrind $rootdir/lib/util/crc16.c/crc16_ut
$valgrind $rootdir/lib/util/crc32_ieee.c/crc32_ieee_ut
$valgrind $rootdir/lib/util/crc32c.c/crc32c_ut
$valgrind $rootdir/lib/util/io_channel.c/io_channel_ut
$valgrind $rootdir/lib/util/string.c/string_ut

if [ $(uname -s) = Linux ]; then
$valgrind $rootdir/lib/vhost/vhost.c/vhost_ut
$valgrind $rootdir/lib/vhost/vhost_scsi.c/vhost_scsi_ut
$valgrind $rootdir/lib/vhost/vhost_blk.c/vhost_blk_ut
fi

# local unit test coverage
if [ "$cov_avail" = "yes" ]; then
	$LCOV -q -d . -c -t "$(hostname)" -o $UT_COVERAGE/ut_cov_test.info
	$LCOV -q -a $UT_COVERAGE/ut_cov_base.info -a $UT_COVERAGE/ut_cov_test.info -o $UT_COVERAGE/ut_cov_total.info
	$LCOV -q -a $UT_COVERAGE/ut_cov_total.info -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info 'app/*' -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info 'examples/*' -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info 'include/*' -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info '$rootdir/lib/vhost/rte_vhost/*' -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info 'test/*' -o $UT_COVERAGE/ut_cov_unit.info
	rm -f $UT_COVERAGE/ut_cov_base.info $UT_COVERAGE/ut_cov_test.info
	genhtml $UT_COVERAGE/ut_cov_unit.info --output-directory $UT_COVERAGE
	git clean -f "*.gcda"
fi

set +x

echo
echo
echo "====================="
echo "All unit tests passed"
echo "====================="
if [ "$cov_avail" = "yes" ]; then
	echo "Note: coverage report is here: ./$UT_COVERAGE"
else
	echo "WARN: lcov not installed or SPDK built without coverage!"
fi
if grep -q '#undef SPDK_CONFIG_ASAN' config.h && [ "$valgrind" = "" ]; then
	echo "WARN: neither valgrind nor ASAN is enabled!"
fi

echo
echo
