#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

if [ ! $(uname -s) = Linux ]; then
	exit 0
fi

source $rootdir/test/nvmf/common.sh

timing_enter nvmf_tgt

trap "exit 1" SIGINT SIGTERM EXIT

TEST_ARGS=$@

run_test suite test/nvmf/target/repro_906.sh $TEST_ARGS

trap - SIGINT SIGTERM EXIT
revert_soft_roce

report_test_completion "nvmf"
timing_exit nvmf_tgt
