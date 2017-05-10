#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/autotest_common.sh

if [ ! $(uname -s) = Linux ] || [ $SPDK_TEST_ISCSI -ne 1 ]; then
	exit 0
fi

export TARGET_IP=127.0.0.1
export INITIATOR_IP=127.0.0.1

timing_enter iscsi_tgt
run_test ./test/iscsi_tgt/calsoft/calsoft.sh
run_test ./test/iscsi_tgt/filesystem/filesystem.sh
run_test ./test/iscsi_tgt/fio/fio.sh
run_test ./test/iscsi_tgt/reset/reset.sh
run_test ./test/iscsi_tgt/rpc_config/rpc_config.sh
run_test ./test/iscsi_tgt/idle_migration/idle_migration.sh
if [ $RUN_NIGHTLY -eq 1 ]; then
	run_test ./test/iscsi_tgt/ip_migration/ip_migration.sh
fi
run_test ./test/iscsi_tgt/ext4test/ext4test.sh
run_test ./test/iscsi_tgt/rbd/rbd.sh
run_test ./test/iscsi_tgt/nvme_remote/fio_remote_nvme.sh
timing_exit iscsi_tgt
