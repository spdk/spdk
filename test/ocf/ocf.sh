#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source $rootdir/test/common/autotest_common.sh

function suite()
{
	run_test suite "ocf_$(basename "$@")" "$@"
}

suite "$testdir/integrity/fio-modes.sh"
suite "$testdir/integrity/bdevperf-iotypes.sh"
suite "$testdir/integrity/stats.sh"
suite "$testdir/management/create-destruct.sh"
suite "$testdir/management/multicore.sh"
suite "$testdir/management/persistent-metadata.sh"
suite "$testdir/management/remove.sh"

report_test_completion "ocf"
