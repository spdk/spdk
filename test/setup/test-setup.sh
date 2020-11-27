#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")
source "$testdir/common.sh"

[[ $(uname -s) == Linux ]] || exit 0

run_test "acl" "$testdir/acl.sh"
run_test "hugepages" "$testdir/hugepages.sh"
run_test "driver" "$testdir/driver.sh"
run_test "devices" "$testdir/devices.sh"
