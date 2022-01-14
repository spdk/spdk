#!/usr/bin/env bash
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

run_test "iscsi_tgt_sock" ./test/iscsi_tgt/sock/sock.sh
if [[ -d /usr/local/calsoft ]]; then
	run_test "iscsi_tgt_calsoft" ./test/iscsi_tgt/calsoft/calsoft.sh
else
	skip_run_test_with_warning "WARNING: Calsoft binaries not found, skipping test!"
fi
run_test "iscsi_tgt_filesystem" ./test/iscsi_tgt/filesystem/filesystem.sh
run_test "iscsi_tgt_reset" ./test/iscsi_tgt/reset/reset.sh
run_test "iscsi_tgt_rpc_config" ./test/iscsi_tgt/rpc_config/rpc_config.sh
run_test "iscsi_tgt_iscsi_lvol" ./test/iscsi_tgt/lvol/iscsi_lvol.sh
run_test "iscsi_tgt_fio" ./test/iscsi_tgt/fio/fio.sh
run_test "iscsi_tgt_qos" ./test/iscsi_tgt/qos/qos.sh
run_test "iscsi_tgt_ip_migration" ./test/iscsi_tgt/ip_migration/ip_migration.sh
run_test "iscsi_tgt_trace_record" ./test/iscsi_tgt/trace_record/trace_record.sh
run_test "iscsi_tgt_login_redirection" ./test/iscsi_tgt/login_redirection/login_redirection.sh

if [ $RUN_NIGHTLY -eq 1 ]; then
	if [ $SPDK_TEST_PMDK -eq 1 ]; then
		run_test "iscsi_tgt_pmem" ./test/iscsi_tgt/pmem/iscsi_pmem.sh 4096 10
	fi
	run_test "iscsi_tgt_ext4test" ./test/iscsi_tgt/ext4test/ext4test.sh
	run_test "iscsi_tgt_digests" ./test/iscsi_tgt/digests/digests.sh
fi
if [ $SPDK_TEST_RBD -eq 1 ]; then
	if ! hash ceph; then
		echo "ERROR: SPDK_TEST_RBD requested but no ceph installed!"
		false
	fi
	run_test "iscsi_tgt_rbd" ./test/iscsi_tgt/rbd/rbd.sh
fi

trap 'cleanup_veth_interfaces; exit 1' SIGINT SIGTERM EXIT

if [ $SPDK_TEST_NVMF -eq 1 ]; then
	# Test configure remote NVMe device from rpc and conf file
	run_test "iscsi_tgt_fio_remote_nvme" ./test/iscsi_tgt/nvme_remote/fio_remote_nvme.sh
fi

if [ $RUN_NIGHTLY -eq 1 ]; then
	run_test "iscsi_tgt_fuzz" ./test/iscsi_tgt/fuzz/fuzz.sh
	run_test "iscsi_tgt_multiconnection" ./test/iscsi_tgt/multiconnection/multiconnection.sh
fi

if [ $SPDK_TEST_ISCSI_INITIATOR -eq 1 ]; then
	run_test "iscsi_tgt_initiator" ./test/iscsi_tgt/initiator/initiator.sh
	run_test "iscsi_tgt_bdev_io_wait" ./test/iscsi_tgt/bdev_io_wait/bdev_io_wait.sh
	run_test "iscsi_tgt_resize" ./test/iscsi_tgt/resize/resize.sh
fi

cleanup_veth_interfaces
trap - SIGINT SIGTERM EXIT
