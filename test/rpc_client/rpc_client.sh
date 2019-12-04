#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

$rootdir/test/rpc_client/rpc_client_test

trap - SIGINT SIGTERM EXIT
