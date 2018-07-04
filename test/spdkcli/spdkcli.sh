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
	killprocess $spdk_tgt_pid
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

function run_rbd_test() {
	trap 'rbd_cleanup; on_error_exit' ERR
	run_spdk_tgt
	rootdir=$(readlink -f $SPDK_BUILD_DIR)
	rbd_setup 127.0.0.1

	python3.5 $SPDK_BUILD_DIR/test/spdkcli/spdkcli_job.py -job load_spdk_tgt_rbd
        python3.5 $SPDK_BUILD_DIR/test/spdkcli/spdkcli_job.py -job clear_spdk_tgt_rbd
	rbd_cleanup
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
	-r|--rbd)
		run_rbd_test
		;;
	*)
                echo "unknown test type: $1"
                exit 1
        ;;
esac

timing_exit spdk_cli
report_test_completion spdk_cli
