#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/scripts/autotest_common.sh

timing_enter scsi

$testdir/dev/dev_ut
$testdir/lun/lun_ut
$testdir/scsi_bdev/scsi_bdev_ut

timing_exit scsi
