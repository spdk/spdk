#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir="$testdir/../../.."
source $rootdir/scripts/autotest_common.sh

$valgrind $testdir/unit/nvme_ns_cmd_c/nvme_ns_cmd_ut
$valgrind $testdir/unit/nvme_c/nvme_ut
$valgrind $testdir/unit/nvme_qpair_c/nvme_qpair_ut
$valgrind $testdir/unit/nvme_ctrlr_c/nvme_ctrlr_ut
$valgrind $testdir/unit/nvme_ctrlr_cmd_c/nvme_ctrlr_cmd_ut

$testdir/aer/aer
process_core

$rootdir/examples/nvme/identify/identify
process_core

$rootdir/examples/nvme/perf/perf -q 128 -w read -s 4096 -t 5
process_core
