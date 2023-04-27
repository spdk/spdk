#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation
#  All rights reserved.
#

NVME_CMD="/usr/local/src/nvme-cli/nvme"
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/common/nvme/functions.sh"

rpc_py=$rootdir/scripts/rpc.py
