#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
rpc_py="$rootdir/scripts/rpc.py"

source $rootdir/test/common/autotest_common.sh

timing_enter cas

for dir in $(ls -1 -d $testdir/*/ | xargs basename); do
	timing_enter $dir
	$testdir/$dir/$dir.sh
	timing_exit $dir
done

timing_exit cas
