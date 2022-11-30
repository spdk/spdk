#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation
#  All rights reserved.
#

set -e

# If the configuration of tests is not provided, no tests will be carried out.
if [[ ! -f $1 ]]; then
	echo "ERROR: SPDK test configuration not specified"
	exit 1
fi

rootdir=$(readlink -f $(dirname $0))

source "$1"
source "$rootdir/test/common/autobuild_common.sh"

SPDK_TEST_AUTOBUILD=${SPDK_TEST_AUTOBUILD:-}
umask 022
cd $rootdir

# Print some test system info out for the log
date -u
git describe --tags

if [ $SPDK_RUN_ASAN -eq 1 ]; then
	run_test "asan" echo "using asan"
fi

if [ $SPDK_RUN_UBSAN -eq 1 ]; then
	run_test "ubsan" echo "using ubsan"
fi

if [ -n "$SPDK_TEST_NATIVE_DPDK" ]; then
	run_test "build_native_dpdk" build_native_dpdk
fi

case "$SPDK_TEST_AUTOBUILD" in
	full)
		./configure $config_params
		echo "** START ** Info for Hostname: $HOSTNAME"
		uname -a
		$MAKE cc_version
		$MAKE cxx_version
		echo "** END ** Info for Hostname: $HOSTNAME"
		;;
	ext | tiny | "") ;;
	*)
		echo "ERROR: supported values for SPDK_TEST_AUTOBUILD are 'full', 'tiny' and 'ext'"
		exit 1
		;;
esac

if [[ $SPDK_TEST_OCF -eq 1 ]]; then
	run_test "autobuild_ocf_precompile" ocf_precompile
fi

if [[ $SPDK_TEST_FUZZER -eq 1 ]]; then
	run_test "autobuild_llvm_precompile" llvm_precompile
fi

if [[ -n $SPDK_TEST_AUTOBUILD ]]; then
	run_test "autobuild" autobuild_test_suite $1
elif [[ $SPDK_TEST_UNITTEST -eq 1 ]]; then
	run_test "unittest_build" unittest_build
elif [[ $SPDK_TEST_SCANBUILD -eq 1 ]]; then
	run_test "scanbuild_make" scanbuild_make
else
	if [[ $SPDK_TEST_FUZZER -eq 1 ]]; then
		# if we are testing nvmf fuzz with llvm lib, --with-shared will cause lib link fail
		./configure $config_params
	else
		# if we aren't testing the unittests, build with shared objects.
		./configure $config_params --with-shared
	fi
	run_test "make" $MAKE $MAKEFLAGS
fi
