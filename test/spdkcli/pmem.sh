#!/usr/bin/env bash
set -xe

MATCH_FILE="spdkcli_pmem.test"
SPDKCLI_BRANCH="/bdevs/pmemblk"
testdir=$(readlink -f $(dirname $0))
. $testdir/common.sh

timing_enter spdkcli_pmem
trap 'on_error_exit;' ERR

timing_enter run_spdk_tgt
run_spdk_tgt
timing_exit run_spdk_tgt

timing_enter spdkcli_create_pmem_config
$spdkcli_job "/bdevs/pmemblk create_pmem_pool /tmp/sample_pmem 32 512" "" True
$spdkcli_job "/bdevs/pmemblk create /tmp/sample_pmem pmem_bdev" "pmem_bdev" True
timing_exit spdkcli_create_pmem_config

timing_enter spdkcli_check_match
check_match
timing_exit spdkcli_check_match

timing_enter spdkcli_clear_pmem_config
$spdkcli_job "/bdevs/pmemblk delete pmem_bdev" "pmem_bdev"
$spdkcli_job "/bdevs/pmemblk delete_pmem_pool /tmp/sample_pmem" ""
rm -f /tmp/sample_pmem
timing_exit spdkcli_clear_pmem_config

killprocess $spdk_tgt_pid
timing_exit spdkcli_pmem
report_test_completion spdk_cli
