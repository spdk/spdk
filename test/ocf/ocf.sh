#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source $rootdir/test/common/autotest_common.sh

run_test "case" "ocf_fio_modes" "$testdir/integrity/fio-modes.sh"
run_test "case" "ocf_bdevperf_iotypes" "$testdir/integrity/bdevperf-iotypes.sh"
run_test "case" "ocf_stats" "$testdir/integrity/stats.sh"
run_test "case" "ocf_create_destruct" "$testdir/management/create-destruct.sh"
run_test "case" "ocf_multicore" "$testdir/management/multicore.sh"
run_test "case" "ocf_persistent_metadata" "$testdir/management/persistent-metadata.sh"
run_test "case" "ocf_remove" "$testdir/management/remove.sh"

report_test_completion "ocf"
