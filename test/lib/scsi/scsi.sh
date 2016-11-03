#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/scripts/autotest_common.sh

timing_enter scsi

$valgrind $testdir/dev/dev_ut
$testdir/init/init_ut
$valgrind $testdir/lun/lun_ut
$testdir/scsi_bdev/scsi_bdev_ut
$valgrind $testdir/scsi_nvme/scsi_nvme_ut

timing_exit scsi
