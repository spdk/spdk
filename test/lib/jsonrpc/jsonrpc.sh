#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/scripts/autotest_common.sh

timing_enter jsonrpc

$testdir/server/jsonrpc_server_ut

timing_exit jsonrpc
