#!/usr/bin/env bash

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")

source "$rootdir/test/common/autotest_common.sh"
source "$testdir/isolate_cores.sh"

"$rootdir/scripts/setup.sh"

run_test "idle" "$testdir/idle.sh"
