#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2016 Intel Corporation
#  All rights reserved.
#

set -e

rootdir=$(readlink -f $(dirname $0))

default_conf=~/autorun-spdk.conf
conf=${1:-${default_conf}}

# If the configuration of tests is not provided, no tests will be carried out.
if [[ ! -f $conf ]]; then
	echo "ERROR: $conf doesn't exist"
	exit 1
fi
source "$conf"

echo "Test configuration:"
cat "$conf"

# Runs agent scripts
$rootdir/autobuild.sh "$conf"
if ((SPDK_TEST_UNITTEST == 1 || SPDK_RUN_FUNCTIONAL_TEST == 1)); then
	sudo -E $rootdir/autotest.sh "$conf"
fi

if [[ $SPDK_TEST_AUTOBUILD != 'tiny' ]]; then
	$rootdir/autopackage.sh "$conf"
fi
