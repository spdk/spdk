#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source $rootdir/test/common/autotest_common.sh

run_test "ocf_fio_modes" "$testdir/integrity/fio-modes.sh"
run_test "ocf_bdevperf_iotypes" "$testdir/integrity/bdevperf-iotypes.sh"
run_test "ocf_stats" "$testdir/integrity/stats.sh"
run_test "ocf_flush" "$testdir/integrity/flush.sh"
run_test "ocf_create_destruct" "$testdir/management/create-destruct.sh"
run_test "ocf_multicore" "$testdir/management/multicore.sh"
run_test "ocf_persistent_metadata" "$testdir/management/persistent-metadata.sh"
run_test "ocf_remove" "$testdir/management/remove.sh"
run_test "ocf_configuration_change" "$testdir/management/configuration-change.sh"
