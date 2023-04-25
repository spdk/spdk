#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  All rights reserved.
#

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../..")
source "$rootdir/test/nvme/cuse/common.sh"

"$rootdir/scripts/setup.sh" reset

scan_nvme_ctrls
ctrl=$(get_ctrl_with_fdp) bdf=${bdfs["$ctrl"]}

"$rootdir/scripts/setup.sh"

run_test "nvme_flexible_data_placement" "$testdir/fdp/fdp" \
	-r "trtype:pcie traddr:$bdf"
