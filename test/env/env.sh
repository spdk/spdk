#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

run_test "env_memory" $testdir/memory/memory_ut
run_test "env_vtophys" $testdir/vtophys/vtophys
run_test "env_pci" $testdir/pci/pci_ut

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
run_test "env_dpdk_post_init" $testdir/env_dpdk_post_init/env_dpdk_post_init $argv

if [ $(uname) = Linux ]; then
	# This tests the --match-allocations DPDK parameter which is only
	# supported on Linux
	run_test "env_mem_callbacks" $testdir/mem_callbacks/mem_callbacks
fi
