#!/usr/bin/env bash
CONVERTER_DIR=$(readlink -f $(dirname $0))
source $CONVERTER_DIR/common.sh

trap 'on_error_exit' ERR

if hash pmempool; then
	rm -f /tmp/sample_pmem
	pmempool create blk --size=32M 512 /tmp/sample_pmem
fi
cp $CONVERTER_DIR/config_pmem.ini $CONVERTER_DIR/config_tmp.ini
run_tests
test_cleanup
