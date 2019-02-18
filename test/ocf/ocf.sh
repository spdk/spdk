#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source $rootdir/test/common/autotest_common.sh

function suite()
{
	timing_enter "$@"
	run_test suite "$@"
	timing_exit "$@"
}

timing_enter ocf

suite "$testdir/integrity/fio-mallocs.sh"
suite "$testdir/integrity/bdevperf-iotypes.sh"
suite "$testdir/management/create-destruct.sh"
suite "$testdir/management/multicore.sh"

timing_exit ocf
