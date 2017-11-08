#!/usr/bin/env bash

SYSTEM=`uname -s`
if [ $SYSTEM = "FreeBSD" ] ; then
    echo "Blobstore.sh can not run in FreeBSD temporarily."
    exit 0
fi

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/autotest_common.sh

timing_enter blobstore

set -e

# Nvme0 target configuration
$rootdir/scripts/gen_nvme.sh > $testdir/blobcli.conf

$rootdir/examples/blob/cli/blobcli -c $testdir/blobcli.conf -b Nvme0n1 -T $testdir/test.bs
find /home -name "M.blob"
pwd
ls -al $testdir
rm -rf $testdir/blobcli.conf
rm -rf $testdir/*.blob
ls -al $testdir

timing_exit blobstore
