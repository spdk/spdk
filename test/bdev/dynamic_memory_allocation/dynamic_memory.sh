#!/usr/bin/env bash
set -ex
DYNAMIC_ALLOCATION_DIR=$(readlink -f $(dirname ${BASH_SOURCE[0]}))
SPDK_BUILD_DIR=$DYNAMIC_ALLOCATION_DIR/../../../
. $DYNAMIC_ALLOCATION_DIR/../../common/autotest_common.sh

spdk_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/spdk.sock"

function run_spdk_tgt() {
	cp $DYNAMIC_ALLOCATION_DIR/spdk_tgt.conf.base $DYNAMIC_ALLOCATION_DIR/spdk_tgt.conf
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $DYNAMIC_ALLOCATION_DIR/spdk_tgt.conf

	echo "Running spdk target"
	$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x1 -p 0 -c $DYNAMIC_ALLOCATION_DIR/spdk_tgt.conf -s 32 -r /var/tmp/spdk.sock &
	spdk_tgt_pid=$!

	echo "Waiting for app to run..."
	waitforlisten $spdk_tgt_pid
	echo "spdk_tgt started - pid=$spdk_tgt_pid"

	rm $DYNAMIC_ALLOCATION_DIR/spdk_tgt.conf
	echo ""
}

function create_malloc_bdev() {
	$spdk_rpc_py construct_malloc_bdev 128 512
}

function create_bdevperf() {
	$DYNAMIC_ALLOCATION_DIR/../bdevperf/bdevperf -c bdevperf_dynamic.conf -q 128 -s 1024 -d 0 -w randread -t 5
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
create_malloc_bdev
killprocess $spdk_tgt_pid
create_bdevperf
