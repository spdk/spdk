#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source "$rootdir/test/nvme/cuse/common.sh"

# We do know that QEMU provides SCC emulation by very default so pin this test
# to it without requesting an extra configuration flag.
[[ $(uname) == Linux && $(< /sys/class/dmi/id/chassis_vendor) == QEMU ]] || exit 0

"$rootdir/scripts/setup.sh" reset

scan_nvme_ctrls
ctrl=$(get_ctrl_with_feature scc) bdf=${bdfs["$ctrl"]}

"$rootdir/scripts/setup.sh"

run_test "nvme_simple_copy" "$testdir/simple_copy/simple_copy" \
	-r "trtype:pcie traddr:$bdf"
