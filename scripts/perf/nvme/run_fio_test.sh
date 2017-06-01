#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../../..)

# Bind devices to NVMe driver
$rootdir/scripts/setup.sh

# Run Performance Test with 1 SSD
python run_fio_test.py $rootdir/test/lib/nvme/performance/fio_test.conf $rootdir/examples/nvme/fio_plugin/fio_plugin 1

# 2 SSDs test run
python run_fio_test.py $rootdir/test/lib/nvme/performance/fio_test.conf $rootdir/examples/nvme/fio_plugin/fio_plugin 2

# 4 SSDs test run
python run_fio_test.py $rootdir/test/lib/nvme/performance/fio_test.conf $rootdir/examples/nvme/fio_plugin/fio_plugin 4

# 8 SSDs test run
python run_fio_test.py $rootdir/test/lib/nvme/performance/fio_test.conf $rootdir/examples/nvme/fio_plugin/fio_plugin 8
