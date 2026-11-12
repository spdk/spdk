#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

run_test "python_rpc_client" python3 "$testdir/rpc_client_test.py"
$rootdir/test/rpc_client/rpc_client_test

trap - SIGINT SIGTERM EXIT
