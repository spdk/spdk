#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

if [ ! $(uname -s) = Linux ]; then
	exit 0
fi

source $rootdir/test/iscsi_tgt/common.sh

# Run cleanup once to make sure we remove any stale iscsiadm
# entries if they were missed in previous runs
iscsicleanup

# Network configuration
create_veth_interfaces

trap 'cleanup_veth_interfaces; exit 1' SIGINT SIGTERM EXIT

run_test "iscsi_tgt_sock" $rootdir/test/iscsi_tgt/sock/sock.sh
if [[ -d /usr/local/calsoft ]]; then
	run_test "iscsi_tgt_calsoft" $rootdir/test/iscsi_tgt/calsoft/calsoft.sh
else
	skip_run_test_with_warning "WARNING: Calsoft binaries not found, skipping test!"
fi
run_test "iscsi_tgt_filesystem" $rootdir/test/iscsi_tgt/filesystem/filesystem.sh
run_test "iscsi_tgt_reset" $rootdir/test/iscsi_tgt/reset/reset.sh
run_test "iscsi_tgt_rpc_config" $rootdir/test/iscsi_tgt/rpc_config/rpc_config.sh
run_test "iscsi_tgt_iscsi_lvol" $rootdir/test/iscsi_tgt/lvol/iscsi_lvol.sh
run_test "iscsi_tgt_fio" $rootdir/test/iscsi_tgt/fio/fio.sh
run_test "iscsi_tgt_qos" $rootdir/test/iscsi_tgt/qos/qos.sh
run_test "iscsi_tgt_ip_migration" $rootdir/test/iscsi_tgt/ip_migration/ip_migration.sh
run_test "iscsi_tgt_trace_record" $rootdir/test/iscsi_tgt/trace_record/trace_record.sh
run_test "iscsi_tgt_login_redirection" $rootdir/test/iscsi_tgt/login_redirection/login_redirection.sh

if [ $RUN_NIGHTLY -eq 1 ]; then
	run_test "iscsi_tgt_ext4test" $rootdir/test/iscsi_tgt/ext4test/ext4test.sh
	run_test "iscsi_tgt_digests" $rootdir/test/iscsi_tgt/digests/digests.sh
fi
if [ $SPDK_TEST_RBD -eq 1 ]; then
	if ! hash ceph; then
		echo "ERROR: SPDK_TEST_RBD requested but no ceph installed!"
		false
	fi
	run_test "iscsi_tgt_rbd" $rootdir/test/iscsi_tgt/rbd/rbd.sh
fi

trap 'cleanup_veth_interfaces; exit 1' SIGINT SIGTERM EXIT

if [ $SPDK_TEST_NVMF -eq 1 ]; then
	# Test configure remote NVMe device from rpc and conf file
	run_test "iscsi_tgt_fio_remote_nvme" $rootdir/test/iscsi_tgt/nvme_remote/fio_remote_nvme.sh
fi

if [ $RUN_NIGHTLY -eq 1 ]; then
	run_test "iscsi_tgt_fuzz" $rootdir/test/iscsi_tgt/fuzz/fuzz.sh
	run_test "iscsi_tgt_multiconnection" $rootdir/test/iscsi_tgt/multiconnection/multiconnection.sh
fi

if [ $SPDK_TEST_ISCSI_INITIATOR -eq 1 ]; then
	run_test "iscsi_tgt_initiator" $rootdir/test/iscsi_tgt/initiator/initiator.sh
	run_test "iscsi_tgt_bdev_io_wait" $rootdir/test/iscsi_tgt/bdev_io_wait/bdev_io_wait.sh
	run_test "iscsi_tgt_resize" $rootdir/test/iscsi_tgt/resize/resize.sh
fi

cleanup_veth_interfaces
trap - SIGINT SIGTERM EXIT
