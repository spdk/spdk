#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)

pushd $testdir

cp spdk.conf.in spdk.conf
$rootdir/scripts/gen_nvme.sh >> spdk.conf

$rootdir/test/lib/blobfs/mkfs/mkfs spdk.conf Nvme0n1
fio jobfile

popd

rm -f spdk.conf
