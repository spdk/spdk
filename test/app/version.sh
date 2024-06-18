#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2023 Intel Corporation
# All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source "$rootdir/test/common/autotest_common.sh"

get_header_version() {
	grep -E "^#define SPDK_VERSION_${1^^}[[:space:]]+" "$rootdir/include/spdk/version.h" \
		| cut -f2 | tr -d \"
}

major=$(get_header_version major)
minor=$(get_header_version minor)
patch=$(get_header_version patch)
suffix=$(get_header_version suffix)

version="${major}.${minor}"

# If patch is zero, we don't keep it in the version
((patch != 0)) && version="${version}.${patch}"
# In python world, the version format is a little different than what we use (see PEP 440), so we
# need to replace "-pre" with "rc0"
version="${version}${suffix/-pre/rc0}"

PYTHONPATH="$PYTHONPATH:$rootdir/python" py_version=$(python3 -c 'import spdk; print(spdk.__version__)')
[[ "$py_version" == "$version" ]]
