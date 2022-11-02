#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

run_test "ioat_perf" $SPDK_EXAMPLE_DIR/ioat_perf -t 1

run_test "ioat_verify" $SPDK_EXAMPLE_DIR/verify -t 1
