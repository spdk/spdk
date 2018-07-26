#!/usr/bin/env bash
CONVERTER_DIR=$(readlink -f $(dirname $0))
source $CONVERTER_DIR/common.sh
source $CONVERTER_DIR/../nvmf/common.sh

trap 'on_error_exit' ERR

create_aio_bdevs
nvme_address=$(lspci -nn -D | grep '8086:5845' | head -1 | awk '{print $1;}')
cp $CONVERTER_DIR/config.ini $CONVERTER_DIR/config_tmp.ini
sed -i "s/0000:00:01.0/$nvme_address/g" $CONVERTER_DIR/config_tmp.ini
run_spdk_tgt
$spdk_rpc_py save_config --filename $CONVERTER_DIR/config.json
sed -i '$!N;s/,.*\"uuid\".*//;P;D' $CONVERTER_DIR/config.json
cat $CONVERTER_DIR/config_tmp.ini | $SPDK_BUILD_DIR/scripts/config_converter.py > $CONVERTER_DIR/config_c.json
diff -I "cpumask" -I "max_queue_depth" -I "queue_depth" <(jq -S . $CONVERTER_DIR/config.json) <(jq -S . $CONVERTER_DIR/config_c.json)
#diff $CONVERTER_DIR/config.json $CONVERTER_DIR/config_c.json
run_initiator
$virtio_rpc_py save_config --filename $CONVERTER_DIR/config_virtio.json
cat $CONVERTER_DIR/config_virtio.ini | $SPDK_BUILD_DIR/scripts/config_converter.py > $CONVERTER_DIR/config_virtio_c.json
diff <(jq -S . $CONVERTER_DIR/config_virtio.json) <(jq -S . $CONVERTER_DIR/config_virtio_c.json)
killprocess $virtio_pid
run_initiator_and_load_json
killprocess $virtio_pid
virtio_pid=""
killprocess $spdk_tgt_pid
run_spdk_tgt_and_load_json
test_cleanup
