#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
SPDK_BUILD_DIR=$(readlink -f $testdir/../..)
source $SPDK_BUILD_DIR/test/common/autotest_common.sh
match_file=$testdir/spdkcli.test.match
match_file_details=$testdir/spdkcli_details.test.match
out_file=$testdir/spdkcli.test
out_file_details=$testdir/spdkcli_details.test

function on_error_exit() {
	set +e
	if [ ! -z $virtio_pid ]; then
		killprocess $virtio_pid
	fi
	if [ ! -z $spdk_tgt_pid ]; then
		killprocess $spdk_tgt_pid
	fi
	rm -f $out_file $out_file_details /tmp/sample_aio
	print_backtrace
	exit 1
}

trap 'on_error_exit' ERR

function run_spdk_tgt() {
	$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x3 -p 0 -s 1024 -r /var/tmp/spdk.sock &
	spdk_tgt_pid=$!

	waitforlisten $spdk_tgt_pid
}

function run_virtio_initiator() {
        $SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x2 -p 0 -g -u -s 1024 -r /var/tmp/virtio.sock &
        virtio_pid=$!

        waitforlisten $virtio_pid /var/tmp/virtio.sock
}

function run_bdevs_test() {
	run_spdk_tgt
	$SPDK_BUILD_DIR/scripts/spdkcli.py ll / > $out_file

	dd if=/dev/zero of=/tmp/sample_aio bs=2048 count=5000
	python3.5 $SPDK_BUILD_DIR/test/spdkcli/spdkcli_job.py -job load_spdk_tgt
	$SPDK_BUILD_DIR/scripts/rpc.py get_vhost_controllers
	$SPDK_BUILD_DIR/scripts/spdkcli.py ll > $out_file
	$SPDK_BUILD_DIR/scripts/spdkcli.py bdevs/split_disk/Nvme0n1p0 show_details  > $out_file_details
	$SPDK_BUILD_DIR/test/app/match/match -v $match_file
	$SPDK_BUILD_DIR/test/app/match/match -v $match_file_details
	python3.5 $SPDK_BUILD_DIR/test/spdkcli/spdkcli_job.py -job clear_spdk_tgt

	rm -f $out_file $out_file_details /tmp/sample_aio
	killprocess $spdk_tgt_pid
}

function run_pmem_test() {
	run_spdk_tgt

	python3.5 $SPDK_BUILD_DIR/test/spdkcli/spdkcli_job.py -job load_spdk_tgt_pmem
	python3.5 $SPDK_BUILD_DIR/test/spdkcli/spdkcli_job.py -job clear_spdk_tgt_pmem
}

function run_virtio_test() {
	run_spdk_tgt
	run_virtio_initiator

	python3.5 $SPDK_BUILD_DIR/test/spdkcli/spdkcli_job.py -job load_spdk_tgt_virtio
	python3.5 $SPDK_BUILD_DIR/test/spdkcli/spdkcli_job.py -job clear_spdk_tgt_virtio
	rm -f $out_file $out_file_details /tmp/sample_aio
	killprocess $spdk_tgt_pid
	killprocess $virtio_pid
}

case $1 in
        -h|--help)
                echo "usage: $(basename $0) TEST_TYPE"
                echo "Test type can be:"
esac

timing_enter spdk_cli

case $1 in
        -b|--bdevs)
		run_bdevs_test
                ;;
        -p|--pmem)
		run_pmem_test
                ;;
	-v|--virtio)
		run_virtio_test
		;;
        *)
                echo "unknown test type: $1"
                exit 1
        ;;
esac

timing_exit spdk_cli
report_test_completion spdk_cli
