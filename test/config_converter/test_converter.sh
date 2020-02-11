#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")
source "$rootdir/test/common/autotest_common.sh"

CONVERTER_DIR=$testdir
SPDK_BUILD_DIR=$rootdir

function test_cleanup() {
	rm -f $CONVERTER_DIR/config_converter.json $CONVERTER_DIR/config_virtio_converter.json
}

function on_error_exit() {
	set +e
	test_cleanup
	print_backtrace
	exit 1
}

trap 'on_error_exit' ERR

$SPDK_BUILD_DIR/scripts/config_converter.py < $CONVERTER_DIR/config.ini > $CONVERTER_DIR/config_converter.json
$SPDK_BUILD_DIR/scripts/config_converter.py < $CONVERTER_DIR/config_virtio.ini > $CONVERTER_DIR/config_virtio_converter.json
diff -I "cpumask" -I "max_queue_depth" -I "queue_depth" <(jq -S . $CONVERTER_DIR/config_converter.json) <(jq -S . $CONVERTER_DIR/spdk_config.json)
diff <(jq -S . $CONVERTER_DIR/config_virtio_converter.json) <(jq -S . $CONVERTER_DIR/spdk_config_virtio.json)
test_cleanup
