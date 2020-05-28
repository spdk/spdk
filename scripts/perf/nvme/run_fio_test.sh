#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)

# Bind devices to NVMe driver
$rootdir/scripts/setup.sh

# Run Performance Test with 1 SSD
$testdir/run_fio_test.py $testdir/fio_test.conf $rootdir/build/fio/spdk_nvme 1

# 2 SSDs test run
$testdir/run_fio_test.py $testdir/fio_test.conf $rootdir/build/fio/spdk_nvme 2

# 4 SSDs test run
$testdir/run_fio_test.py $testdir/fio_test.conf $rootdir/build/fio/spdk_nvme 4

# 8 SSDs test run
$testdir/run_fio_test.py $testdir/fio_test.conf $rootdir/build/fio/spdk_nvme 8
