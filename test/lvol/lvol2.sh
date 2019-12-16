#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

timing_enter lvol

timing_enter basic
run_test "lvol_basic" test/lvol/basic.sh
run_test "lvol_resize" test/lvol/resize.sh
run_test "lvol_hotremove" test/lvol/hotremove.sh
run_test "lvol_tasting" test/lvol/tasting.sh
run_test "lvol_snapshot_clone" test/lvol/snapshot_clone.sh
run_test "lvol_rename" test/lvol/rename.sh
run_test "lvol_provisioning" test/lvol/thin_provisioning.sh
timing_exit basic

timing_exit lvol
