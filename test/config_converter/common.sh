#!/usr/bin/env bash
CONVERTER_DIR=$(readlink -f $(dirname $0))
SPDK_BUILD_DIR=$CONVERTER_DIR/../../
source $CONVERTER_DIR/../common/autotest_common.sh

spdk_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/spdk.sock"
virtio_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/virtio.sock"

function run_spdk_tgt() {
	echo "Running spdk target"
	$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x1 -p 0 -s 2048 -c $CONVERTER_DIR/config_tmp.ini &
	spdk_tgt_pid=$!

	echo "Waiting for app to run..."
	waitforlisten $spdk_tgt_pid
	echo "spdk_tgt started - pid=$spdk_tgt_pid but waits for subsystem initialization"
	echo ""
}

function run_spdk_tgt_and_load_json() {
	echo "Running spdk target"
	$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x1 -p 0 -s 2048 --wait-for-rpc &
	spdk_tgt_pid=$!

	echo "Waiting for app to run..."
	waitforlisten $spdk_tgt_pid
	echo "spdk_tgt started - pid=$spdk_tgt_pid but waits for subsystem initialization"
	echo ""
	$spdk_rpc_py load_config --filename $CONVERTER_DIR/config_c.json
}

function run_initiator() {
	$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x2 -p 0 -g -u -S -s 1024 -r /var/tmp/virtio.sock -c $CONVERTER_DIR/config_virtio.ini &
	virtio_pid=$!
	waitforlisten $virtio_pid /var/tmp/virtio.sock
}

function run_initiator_and_load_json() {
	$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x2 -p 0 -g -u -S -s 1024 -r /var/tmp/virtio.sock --wait-for-rpc &
	virtio_pid=$!
	waitforlisten $virtio_pid /var/tmp/virtio.sock
	$virtio_rpc_py load_config --filename $CONVERTER_DIR/config_virtio_c.json
}

function test_cleanup() {
	if [ ! -z $virtio_pid ]; then
		killprocess $virtio_pid
	fi
	if [ ! -z $spdk_tgt_pid ]; then
		killprocess $spdk_tgt_pid
	fi
	rm -f $CONVERTER_DIR/config_tmp.ini
	rm -f $CONVERTER_DIR/config.json $CONVERTER_DIR/config_c.json
	rm -f $CONVERTER_DIR/config_virtio.json $CONVERTER_DIR/config_virtio_c.json
	rm -f /tmp/sample_aio0 /tmp/sample_aio1 /tmp/sample_aio2 /tmp/sample_aio3 /tmp/sample_aio4
	rm -f /tmp/sample_pmem
}

function run_tests() {
	run_spdk_tgt
	$spdk_rpc_py save_config --filename $CONVERTER_DIR/config.json
	sed -i '$!N;s/,.*\"uuid\".*//;P;D' $CONVERTER_DIR/config.json
	cat $CONVERTER_DIR/config_tmp.ini | $SPDK_BUILD_DIR/scripts/config_converter.py > $CONVERTER_DIR/config_c.json
	diff -I "cpumask" -I "max_queue_depth" -I "queue_depth" <(jq -S . $CONVERTER_DIR/config.json) <(jq -S . $CONVERTER_DIR/config_c.json)
	killprocess $spdk_tgt_pid
	run_spdk_tgt_and_load_json
}

function create_aio_bdevs() {
	dd if=/dev/zero of=/tmp/sample_aio0 bs=2048 count=2000
	dd if=/dev/zero of=/tmp/sample_aio1 bs=2048 count=2000
	dd if=/dev/zero of=/tmp/sample_aio2 bs=2048 count=2000
	dd if=/dev/zero of=/tmp/sample_aio3 bs=2048 count=2000
	dd if=/dev/zero of=/tmp/sample_aio4 bs=2048 count=2000
}

function on_error_exit() {
	set +e
	test_cleanup
	print_backtrace
	exit 1
}
