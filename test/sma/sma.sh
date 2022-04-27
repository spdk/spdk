#!/usr/bin/env bash

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../..")

source "$rootdir/test/common/autotest_common.sh"

run_test "sma_nvmf_tcp" $testdir/nvmf_tcp.sh
run_test "sma_plugins" $testdir/plugins.sh
run_test "sma_discovery" $testdir/discovery.sh
run_test "sma_vhost" $testdir/vhost_blk.sh
