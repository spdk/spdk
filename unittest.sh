#!/usr/bin/env bash

set -xe

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

make -C test/lib/jsonrpc CONFIG_WERROR=y

test/lib/jsonrpc/server/jsonrpc_server_ut

make -C test/lib/log CONFIG_WERROR=y

test/lib/log/log_ut
