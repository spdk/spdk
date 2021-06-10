#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/common.sh

devices=($(get_ftl_nvme_dev)) device=${devices[0]}

run_test "ftl_bdevperf" $testdir/bdevperf.sh $device
run_test "ftl_bdevperf_append" $testdir/bdevperf.sh $device --use_append
run_test "ftl_restore" $testdir/restore.sh $device
run_test "ftl_json" $testdir/json.sh $device
run_test "ftl_fio" "$testdir/fio.sh" "$device"
