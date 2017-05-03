#!/usr/bin/env bash
#
# Environment variables:
#  $valgrind    Valgrind executable name, if desired

set -xe

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

# TODO: fix valgrind warnings and add $valgrind to iSCSI tests
test/lib/iscsi/param/param_ut
$valgrind test/lib/iscsi/target_node/target_node_ut test/lib/iscsi/target_node/target_node.conf
test/lib/iscsi/pdu/pdu

$valgrind test/lib/util/bit_array/bit_array_ut
$valgrind test/lib/util/io_channel/io_channel_ut
$valgrind test/lib/util/string/string_ut

set +x

echo
echo
echo "====================="
echo "All unit tests passed"
echo "====================="
echo
echo
