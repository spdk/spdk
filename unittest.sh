#!/usr/bin/env bash

set -xe

make CONFIG_WERROR=y

test/lib/nvme/unit/nvme_c/nvme_ut
test/lib/nvme/unit/nvme_ctrlr_c/nvme_ctrlr_ut
test/lib/nvme/unit/nvme_ctrlr_cmd_c/nvme_ctrlr_cmd_ut
test/lib/nvme/unit/nvme_ns_cmd_c/nvme_ns_cmd_ut
test/lib/nvme/unit/nvme_qpair_c/nvme_qpair_ut

test/lib/ioat/unit/ioat_ut

test/lib/json/parse/json_parse_ut
test/lib/json/util/json_util_ut
test/lib/json/write/json_write_ut

test/lib/jsonrpc/server/jsonrpc_server_ut

test/lib/log/log_ut

test/lib/nvmf/request/request_ut
test/lib/nvmf/session/session_ut
test/lib/nvmf/subsystem/subsystem_ut

test/lib/scsi/dev/dev_ut
test/lib/scsi/lun/lun_ut
test/lib/scsi/scsi_bdev/scsi_bdev_ut
test/lib/scsi/scsi_nvme/scsi_nvme_ut

test/lib/util/bit_array/bit_array_ut
test/lib/util/io_channel/io_channel_ut
