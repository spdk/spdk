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

if [ $SPDK_RUN_INSTALLED_DPDK -eq 1 ]; then
	timing_enter env_dpdk_post_init
	$testdir/env_dpdk_post_init/env_dpdk_post_init
	timing_exit env_dpdk_post_init
fi

report_test_completion "env"
timing_exit env
