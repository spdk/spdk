#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

timing_enter env

timing_enter memory
$testdir/memory/memory_ut
timing_exit memory

timing_enter vtophys
$testdir/vtophys/vtophys
timing_exit vtophys

timing_enter pci
$testdir/pci/pci_ut
timing_exit pci

# Do not enable this test yet - it doesn't work with current DPDK.
# We will use this test for fixing DPDK, then enable it once those
# changes are merged upstream.
#timing_enter mem_callbacks
#$testdir/mem_callbacks/mem_callbacks
#timing_exit mem_callbacks

report_test_completion "env"
timing_exit env
