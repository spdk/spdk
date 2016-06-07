#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/scripts/autotest_common.sh

# TODO: unit tests should not depend on library being enabled
if [ ! -f $testdir/nvmf_c/nvmf_ut ]; then
        exit 0
fi

timing_enter nvmf

timing_enter unit
$testdir/nvmf_c/nvmf_ut
timing_exit unit

timing_exit nvmf
