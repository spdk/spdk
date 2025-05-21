#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

if [ ! $(uname -s) = Linux ]; then
	exit 0
fi

run_test "nvmf_sock" $rootdir/test/nvmf/sock/sock.sh --transport=$SPDK_TEST_NVMF_TRANSPORT
run_test "nvmf_target_core" $rootdir/test/nvmf/nvmf_target_core.sh --transport=$SPDK_TEST_NVMF_TRANSPORT
run_test "nvmf_target_extra" $rootdir/test/nvmf/nvmf_target_extra.sh --transport=$SPDK_TEST_NVMF_TRANSPORT
run_test "nvmf_host" $rootdir/test/nvmf/nvmf_host.sh --transport=$SPDK_TEST_NVMF_TRANSPORT

# Interrupt mode for now is supported only on the target, with the TCP transport and posix or ssl socket implementations.
# Disabled due to #3634
#if [[ "$SPDK_TEST_NVMF_TRANSPORT" = "tcp" && $SPDK_TEST_URING -eq 0 ]]; then
#	run_test "nvmf_target_core_interrupt_mode" $rootdir/test/nvmf/nvmf_target_core.sh --transport=$SPDK_TEST_NVMF_TRANSPORT --interrupt-mode
#	run_test "nvmf_interrupt" $rootdir/test/nvmf/target/interrupt.sh --transport=$SPDK_TEST_NVMF_TRANSPORT --interrupt-mode
#fi
