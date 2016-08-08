#!/usr/bin/env bash

set -xe

make config.h CONFIG_WERROR=y
make -C lib/cunit CONFIG_WERROR=y
make -C lib/log CONFIG_WERROR=y

make -C test/lib/nvme/unit CONFIG_WERROR=y

test/lib/nvme/unit/nvme_c/nvme_ut
test/lib/nvme/unit/nvme_ctrlr_c/nvme_ctrlr_ut
test/lib/nvme/unit/nvme_ctrlr_cmd_c/nvme_ctrlr_cmd_ut
test/lib/nvme/unit/nvme_ns_cmd_c/nvme_ns_cmd_ut
test/lib/nvme/unit/nvme_qpair_c/nvme_qpair_ut

make -C test/lib/ioat/unit CONFIG_WERROR=y

test/lib/ioat/unit/ioat_ut

make -C test/lib/json CONFIG_WERROR=y

test/lib/json/parse/json_parse_ut
test/lib/json/util/json_util_ut
test/lib/json/write/json_write_ut

make -C lib/json CONFIG_WERROR=y
make -C test/lib/jsonrpc CONFIG_WERROR=y

test/lib/jsonrpc/server/jsonrpc_server_ut

make -C test/lib/log CONFIG_WERROR=y

test/lib/log/log_ut

make -C test/lib/nvmf CONFIG_WERROR=y

test/lib/nvmf/request/request_ut
test/lib/nvmf/session/session_ut
test/lib/nvmf/subsystem/subsystem_ut

# TODO: allow lib/util to build without DPDK
#make -C test/lib/scsi CONFIG_WERROR=y
make -C test/lib/scsi/dev CONFIG_WERROR=y
make -C test/lib/scsi/lun CONFIG_WERROR=y

test/lib/scsi/dev/dev_ut
test/lib/scsi/lun/lun_ut
#test/lib/scsi/scsi_bdev/scsi_bdev_ut

make -C test/lib/util/bit_array CONFIG_WERROR=y
make -C test/lib/util/io_channel CONFIG_WERROR=y

test/lib/util/bit_array/bit_array_ut
test/lib/util/io_channel/io_channel_ut
