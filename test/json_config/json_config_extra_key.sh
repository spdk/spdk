#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation
#  All rights reserved.
#

rootdir=$(readlink -f $(dirname $0)/../..)
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"
source "$rootdir/test/json_config/common.sh"

# Check that adding arbitrary top-level key to JSON SPDK config alongside
# "subsystems" doesn't break SPDK parsing that occurs when loading config
# to initialize subsystems. This enables applications to use the same config
# file to communicate private and SPDK data.

declare -A app_pid=([target]="")
declare -A app_socket=([target]='/var/tmp/spdk_tgt.sock')
declare -A app_params=([target]='-m 0x1 -s 1024')
declare -A configs_path=([target]="$rootdir/test/json_config/extra_key.json")

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR

echo "INFO: launching applications..."
json_config_test_start_app target --json ${configs_path[target]}

echo "INFO: shutting down applications..."
json_config_test_shutdown_app target

echo "Success"
