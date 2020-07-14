#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")
source "$testdir/common.sh"

"$rootdir/scripts/setup.sh"
nvmes=($(nvme_in_userspace))

check_liburing

run_test "spdk_dd_basic_rw" "$testdir/basic_rw.sh" "${nvmes[@]}"
run_test "spdk_dd_posix" "$testdir/posix.sh"
run_test "spdk_dd_bdev_to_bdev" "$testdir/bdev_to_bdev.sh" "${nvmes[@]}"
