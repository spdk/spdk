#!/usr/bin/env bash
CONVERTER_DIR=$(readlink -f $(dirname $0))
source $CONVERTER_DIR/common.sh

trap 'on_error_exit' ERR

if hash pmempool; then
	rm -f /tmp/sample_pmem
	pmempool create blk --size=32M 512 /tmp/sample_pmem
fi
cp $CONVERTER_DIR/config_pmem.ini $CONVERTER_DIR/config_tmp.ini
run_spdk_tgt
$spdk_rpc_py save_config --filename $CONVERTER_DIR/config.json
sed -i '$!N;s/,.*\"uuid\".*//;P;D' $CONVERTER_DIR/config.json
$SPDK_BUILD_DIR/scripts/config_converter.py -old $CONVERTER_DIR/config_tmp.ini -new $CONVERTER_DIR/config_c.json
cat $CONVERTER_DIR/config_c.json
cat $CONVERTER_DIR/config.json
diff -I "cpumask" -I "max_queue_depth" -I "queue_depth" <(jq -S . $CONVERTER_DIR/config.json) <(jq -S . $CONVERTER_DIR/config_c.json)
killprocess $spdk_tgt_pid
run_spdk_tgt_and_load_json
test_cleanup
