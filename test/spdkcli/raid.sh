#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/spdkcli/common.sh
source $rootdir/test/iscsi_tgt/common.sh

MATCH_FILE="spdkcli_raid.test"
SPDKCLI_BRANCH="/bdevs"
testdir=$(readlink -f $(dirname $0))
. $testdir/common.sh

trap 'on_error_exit;' ERR

timing_enter run_spdk_tgt
run_spdk_tgt
timing_exit run_spdk_tgt

timing_enter spdkcli_create_malloc
$spdkcli_job "'/bdevs/malloc create 8 512 Malloc1' 'Malloc1' True
'/bdevs/malloc create 8 512 Malloc2' 'Malloc2' True
"
timing_exit spdkcli_create_malloc

timing_enter spdkcli_create_raid
$spdkcli_job "'/bdevs/raid_volume create testraid 0 \"Malloc1 Malloc2\" 4' 'testraid' True
"
timing_exit spdkcli_create_raid

timing_enter spdkcli_check_match
check_match
timing_exit spdkcli_check_match

timing_enter spdkcli_delete_raid
$spdkcli_job "'/bdevs/raid_volume delete testraid' '' True
"
timing_exit spdkcli_delete_raid

timing_enter spdkcli_delete_malloc
$spdkcli_job "'/bdevs/malloc delete Malloc1' '' True
'/bdevs/malloc delete Malloc2' '' True
"
timing_exit spdkcli_delete_malloc

killprocess $spdk_tgt_pid
