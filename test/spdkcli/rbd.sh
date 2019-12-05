#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/spdkcli/common.sh

MATCH_FILE="spdkcli_rbd.test"
SPDKCLI_BRANCH="/bdevs/rbd"

trap 'on_error_exit' ERR
timing_enter run_spdk_tgt
run_spdk_tgt
timing_exit run_spdk_tgt

timing_enter spdkcli_create_rbd_config
trap 'rbd_cleanup; on_error_exit' ERR
rbd_cleanup
rbd_setup 127.0.0.1
$spdkcli_job "\"/bdevs/rbd create rbd foo 512' 'Ceph0' True "\"/bdevs/rbd create rbd foo 512 'Ceph1' True"
timing_exit spdkcli_create_rbd_config

timing_enter spdkcli_check_match
check_match
timing_exit spdkcli_check_match

timing_enter spdkcli_clear_rbd_config
$spdkcli_job "\"/bdevs/rbd delete Ceph0 'Ceph0' "\"/bdevs/rbd delete_all' 'Ceph1' "
rbd_cleanup
timing_exit spdkcli_clear_rbd_config

killprocess $spdk_tgt_pid
