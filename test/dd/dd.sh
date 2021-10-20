#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")
source "$testdir/common.sh"

"$rootdir/scripts/setup.sh"
nvmes=($(nvme_in_userspace))

check_liburing

if ((liburing_in_use == 0 && SPDK_TEST_URING == 1)); then
	printf 'SPDK_TEST_URING is set but spdk_dd is not linked to liburing, aborting\n' >&2
	exit 1
fi

run_test "spdk_dd_basic_rw" "$testdir/basic_rw.sh" "${nvmes[@]}"
run_test "spdk_dd_posix" "$testdir/posix.sh"
run_test "spdk_dd_malloc" "$testdir/malloc.sh"
run_test "spdk_dd_bdev_to_bdev" "$testdir/bdev_to_bdev.sh" "${nvmes[@]}"
if ((SPDK_TEST_URING == 1)); then
	run_test "spdk_dd_uring" "$testdir/uring.sh"
fi
