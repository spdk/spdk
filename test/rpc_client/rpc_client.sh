#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

timing_enter rpc_client
$rootdir/test/rpc_client/rpc_client_test
timing_exit rpc_client

trap - SIGINT SIGTERM EXIT
