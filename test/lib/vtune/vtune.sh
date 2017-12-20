#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
VTUNE_DIR=$(readlink -f $testdir/../../../../vtune_codes)

if [ ! -d $VTUNE_DIR ]; then
	echo "No VTune codes in this machine."
	exit 0
fi

timing_enter vtune

trap "exit 1" SIGINT SIGTERM EXIT

cd $rootdir
make clean
./configure --with-vtune=$VTUNE_DIR
make -j 8
cd -

trap - SIGINT SIGTERM EXIT
timing_exit vtune
