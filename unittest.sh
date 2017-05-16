#!/usr/bin/env bash
#
# Environment variables:
#  $valgrind    Valgrind executable name, if desired

set -xe

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

$valgrind test/lib/blob/blob_ut/blob_ut

$valgrind test/lib/blobfs/blobfs_async_ut/blobfs_async_ut
# blobfs_sync_ut hangs when run under valgrind, so don't use $valgrind
test/lib/blobfs/blobfs_sync_ut/blobfs_sync_ut

$valgrind test/lib/nvme/unit/nvme_c/nvme_ut
$valgrind test/lib/nvme/unit/nvme_ctrlr_c/nvme_ctrlr_ut
$valgrind test/lib/nvme/unit/nvme_ctrlr_cmd_c/nvme_ctrlr_cmd_ut
$valgrind test/lib/nvme/unit/nvme_ns_c/nvme_ns_ut
$valgrind test/lib/nvme/unit/nvme_ns_cmd_c/nvme_ns_cmd_ut
$valgrind test/lib/nvme/unit/nvme_qpair_c/nvme_qpair_ut
$valgrind test/lib/nvme/unit/nvme_pcie_c/nvme_pcie_ut
$valgrind test/lib/nvme/unit/nvme_quirks_c/nvme_quirks_ut

$valgrind test/lib/ioat/unit/ioat_ut

$valgrind test/lib/json/parse/json_parse_ut
$valgrind test/lib/json/util/json_util_ut
$valgrind test/lib/json/write/json_write_ut

$valgrind test/lib/jsonrpc/server/jsonrpc_server_ut

$valgrind test/lib/log/log_ut

$valgrind test/lib/nvmf/discovery/discovery_ut
$valgrind test/lib/nvmf/request/request_ut
$valgrind test/lib/nvmf/session/session_ut
$valgrind test/lib/nvmf/subsystem/subsystem_ut
$valgrind test/lib/nvmf/direct/direct_ut
$valgrind test/lib/nvmf/virtual/virtual_ut

$valgrind test/lib/scsi/dev/dev_ut
$valgrind test/lib/scsi/init/init_ut
$valgrind test/lib/scsi/lun/lun_ut
$valgrind test/lib/scsi/scsi_bdev/scsi_bdev_ut
$valgrind test/lib/scsi/scsi_nvme/scsi_nvme_ut

$valgrind test/lib/iscsi/param/param_ut
$valgrind test/lib/iscsi/target_node/target_node_ut test/lib/iscsi/target_node/target_node.conf
$valgrind test/lib/iscsi/pdu/pdu

$valgrind test/unit/lib/util/bit_array.c/bit_array_ut
$valgrind test/lib/util/io_channel/io_channel_ut
$valgrind test/lib/util/string/string_ut

# local unit test coverage
if [ "$cov_avail" = "yes" ]; then
	$LCOV -q -d . -c -t "$(hostname)" -o $UT_COVERAGE/ut_cov_test.info
	$LCOV -q -a $UT_COVERAGE/ut_cov_base.info -a $UT_COVERAGE/ut_cov_test.info -o $UT_COVERAGE/ut_cov_total.info
	$LCOV -q -a $UT_COVERAGE/ut_cov_total.info -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info 'app/*' -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info 'examples/*' -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info 'include/*' -o $UT_COVERAGE/ut_cov_unit.info
	$LCOV -q -r $UT_COVERAGE/ut_cov_unit.info 'lib/vhost/rte_vhost/*' -o $UT_COVERAGE/ut_cov_unit.info
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
echo
echo
