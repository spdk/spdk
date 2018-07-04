#!/usr/bin/env bash
set -xe

MATCH_FILE="spdkcli.test_pmem"
testdir=$(readlink -f $(dirname $0))
. $testdir/common.sh

timing_enter spdk_cli
trap 'on_error_exit' ERR

timing_enter run_spdk_tgt
run_spdk_tgt
timing_exit run_spdk_tgt

timing_enter spdkcli_pmem
$spdkcli_job "/bdevs/pmemblk create_pmem_pool /tmp/sample_pmem 32 512" "" True
$spdkcli_job "/bdevs/pmemblk create/tmp/sample_pmem pmem_bdev" "pmem_bdev" True

$SPDKCLI_BUILD_DIR/scripts/spdkcli.py ll > $testdir/$MATCH_FILE
$SPDKCLI_BUILD_DIR/test/app/match/match -v $testdir/$MATCH_FILE".match"

$spdkcli_job "/bdevs/pmemblk delete pmem_bdev" "pmem_bdev"
$spdkcli_job "/bdevs/pmemblk delete_pmem_pool /tmp/sample_pmem" ""
timing_exit spdkcli_pmem

killprocess $spdk_tgt_pid
rm -f $testdir/spdkcli.test_pmem /tmp/sample_pmem
timing_exit spdk_cli
report_test_completion spdk_cli
