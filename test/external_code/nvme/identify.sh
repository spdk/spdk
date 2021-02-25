#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..

source $rootdir/test/common/autotest_common.sh

export SPDK_LIB_DIR=$rootdir/build/lib
export DPDK_LIB_DIR=${SPDK_RUN_EXTERNAL_DPDK:-$rootdir/dpdk/build}/lib
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$SPDK_LIB_DIR:$DPDK_LIB_DIR:$testdir

# Make sure all NVMe devices are reported if no address is specified
identify_data=$($testdir/identify)
for bdf in $(get_nvme_bdfs); do
	grep $bdf <<< $identify_data
done

# Verify that each device can be queried individually too
for bdf in $(get_nvme_bdfs); do
	$testdir/identify $bdf
done
