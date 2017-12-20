#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
VTUNE_DIR="/home/sys_sgsw/vtune_codes"

if [ ! -d $VTUNE_DIR ]; then
	echo "No VTune codes in this machine."
	exit 0
fi

timing_enter vtune

trap "exit 1" SIGINT SIGTERM EXIT

cd $rootdir
make clean
cp CONFIG.local backconfig
./configure --with-vtune=$VTUNE_DIR
make -j 8
trap - SIGINT SIGTERM EXIT

rm -rf CONFIG.local
mv backconfig CONFIG.local
make clean && make -j 8
cd -

timing_exit vtune
