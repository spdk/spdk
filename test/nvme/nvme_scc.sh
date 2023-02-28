#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

if [ $(uname) = Linux ]; then
	$rootdir/scripts/setup.sh
fi

run_test "nvme_simple_copy" $testdir/simple_copy/simple_copy
