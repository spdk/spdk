#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation
#  All rights reserved.
#

rootdir=$(readlink -f $(dirname $0))
source "$rootdir/test/common/autobuild_common.sh"

if [[ $SPDK_TEST_RELEASE_BUILD -eq 1 ]]; then
	build_release
fi

timing_finish
