#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

timing_enter lvol

timing_enter basic
run_test "lvol_basic" $rootdir/test/lvol/basic.sh
run_test "lvol_resize" $rootdir/test/lvol/resize.sh
run_test "lvol_hotremove" $rootdir/test/lvol/hotremove.sh
run_test "lvol_tasting" $rootdir/test/lvol/tasting.sh
run_test "lvol_snapshot_clone" $rootdir/test/lvol/snapshot_clone.sh
run_test "lvol_rename" $rootdir/test/lvol/rename.sh
run_test "lvol_provisioning" $rootdir/test/lvol/thin_provisioning.sh
run_test "lvol_esnap" $rootdir/test/lvol/esnap/esnap
timing_exit basic

timing_exit lvol
