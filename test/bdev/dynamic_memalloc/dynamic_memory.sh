#!/usr/bin/env bash
set -ex
DYNAMIC_ALLOCATION_DIR=$(readlink -f $(dirname ${BASH_SOURCE[0]}))
SPDK_BUILD_DIR=$DYNAMIC_ALLOCATION_DIR/../../../
. $DYNAMIC_ALLOCATION_DIR/../../common/autotest_common.sh

spdk_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/spdk.sock"

function run_spdk_tgt() {
	echo "Running spdk target"
	$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x1 -p 0 -s 1024 --wait-for-rpc &
	spdk_tgt_pid=$!

	echo "Waiting for app to run..."
	waitforlisten $spdk_tgt_pid
	echo "spdk_tgt started - pid=$spdk_tgt_pid"
	echo ""
}

function load_nvme() {
	echo '{"subsystems": [' > nvme_config.json
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh --json >> nvme_config.json
	echo ']}' >> nvme_config.json
	$spdk_rpc_py load_config < nvme_config.json
	rm nvme_config.json
}

function create_malloc_bdev() {
	$spdk_rpc_py construct_malloc_bdev 1024 512
}

function create_bdevperf() {
	$DYNAMIC_ALLOCATION_DIR/../bdevperf/bdevperf -c $DYNAMIC_ALLOCATION_DIR/bdevperf_dynamic.conf -q 128 -o 512 -s 512 -d 0 -w randread -t 5
}

function on_error_exit() {
        set +e
        echo "Error on $1 - $2"
        killprocess $spdk_tgt_pid
        print_backtrace
        exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
run_spdk_tgt
load_nvme
create_malloc_bdev
killprocess $spdk_tgt_pid
create_bdevperf
