#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh

trap 'killprocess $spdk_tgt_pid; exit 1' ERR

$SPDK_BIN_DIR/spdk_tgt &
spdk_tgt_pid=$!
waitforlisten $spdk_tgt_pid

# Test deprecated rpcs in json
$rootdir/scripts/rpc.py load_config -i < $testdir/conf.json

# Test deprecated rpcs in rpc.py
$rootdir/scripts/rpc.py delete_malloc_bdev "Malloc0"
$rootdir/scripts/rpc.py delete_malloc_bdev "Malloc1"

killprocess $spdk_tgt_pid
