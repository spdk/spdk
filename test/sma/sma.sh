#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../..")

source "$rootdir/test/common/autotest_common.sh"

run_test "sma_nvmf_tcp" $testdir/nvmf_tcp.sh
run_test "sma_vfiouser_qemu" $testdir/vfiouser_qemu.sh
run_test "sma_plugins" $testdir/plugins.sh
run_test "sma_discovery" $testdir/discovery.sh
run_test "sma_vhost" $testdir/vhost_blk.sh
run_test "sma_crypto" $testdir/crypto.sh
run_test "sma_qos" $testdir/qos.sh
