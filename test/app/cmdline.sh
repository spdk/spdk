#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

trap 'killprocess $spdk_tgt_pid; exit 1' ERR

# Add an allowlist here...
$SPDK_BIN_DIR/spdk_tgt &
spdk_tgt_pid=$!
waitforlisten $spdk_tgt_pid

# Do both some positive and negative testing on that allowlist here...
# You can use the NOT() function to help with the negative test
# $rootdir/scripts/rpc.py some_rpc

killprocess $spdk_tgt_pid
