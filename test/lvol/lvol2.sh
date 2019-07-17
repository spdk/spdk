#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

timing_enter lvol

timing_enter basic
run_test "lvol_basic" test/lvol/basic.sh
run_test "lvol_resize" test/lvol/resize.sh
run_test "lvol_hotremove" test/lvol/hotremove.sh
timing_exit basic

timing_exit lvol
