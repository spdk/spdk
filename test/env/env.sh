#!/usr/bin/env bash

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

timing_enter env_dpdk_post_init
argv="-c 0x1 "
if [ $(uname) = Linux ]; then
	# The default base virtaddr falls into a region reserved by ASAN.
	# DPDK will try to find the nearest available address space by
	# trying to do mmap over and over, which will take ages to finish.
	# We speed up the process by specifying an address that's not
	# supposed to be reserved by ASAN. Regular SPDK applications do
	# this implicitly.
	argv+="--base-virtaddr=0x200000000000"
fi
$testdir/env_dpdk_post_init/env_dpdk_post_init $argv
timing_exit env_dpdk_post_init

if [ $(uname) = Linux ]; then
	# This tests the --match-allocations DPDK parameter which is only
	# supported on Linux
	timing_enter mem_callbacks
	$testdir/mem_callbacks/mem_callbacks
	timing_exit mem_callbacks
fi

report_test_completion "env"
timing_exit env
