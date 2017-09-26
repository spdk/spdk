#!/usr/bin/env bash

set -xe

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"

source $TEST_DIR//test/pmem/common/common.sh

nbd_start

cd $TEST_DIR//test/pmem/ && pmempool create -s 32000000 blk 512 pool_file

### pmem_pool_info
if ! $rpc_py pmem_pool_info $TEST_DIR//test/pmem/pool_file; then
	echo "Failed to get pmem_pool_info"
	false
fi

cd $TEST_DIR//test/pmem/ && rm pool_file
nbd_kill
