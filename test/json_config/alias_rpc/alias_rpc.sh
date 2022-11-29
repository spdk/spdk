#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh

trap 'killprocess $spdk_tgt_pid; exit 1' ERR

$SPDK_BIN_DIR/spdk_tgt &
spdk_tgt_pid=$!
waitforlisten $spdk_tgt_pid

# Test deprecated rpcs in json
$rootdir/scripts/rpc.py load_config -i < $testdir/conf.json

killprocess $spdk_tgt_pid
