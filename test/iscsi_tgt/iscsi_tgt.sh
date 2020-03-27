#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

if [ ! $(uname -s) = Linux ]; then
	exit 0
fi

source $rootdir/test/iscsi_tgt/common.sh

# $1 = test type (posix/vpp)
if [ "$1" == "posix" ] || [ "$1" == "vpp" ]; then
	TEST_TYPE=$1
else
	echo "No iSCSI test type specified"
	exit 1
fi

# Run cleanup once to make sure we remove any stale iscsiadm
# entries if they were missed in previous runs
iscsicleanup

# Network configuration
create_veth_interfaces $TEST_TYPE

trap 'cleanup_veth_interfaces $TEST_TYPE; exit 1' SIGINT SIGTERM EXIT

run_test "iscsi_tgt_sock" ./test/iscsi_tgt/sock/sock.sh $TEST_TYPE
if [ "$TEST_TYPE" == "posix" ]; then
	# calsoft doesn't handle TCP stream properly and fails decoding iSCSI
	# requests when are divided by TCP segmentation. This is very common
	# situation for VPP and causes that calsoft.sh never PASS.
	if [[ -d /usr/local/calsoft ]]; then
		run_test "iscsi_tgt_calsoft" ./test/iscsi_tgt/calsoft/calsoft.sh
	else
		skip_run_test_with_warning "WARNING: Calsoft binaries not found, skipping test!"
	fi
fi
run_test "iscsi_tgt_filesystem" ./test/iscsi_tgt/filesystem/filesystem.sh
run_test "iscsi_tgt_reset" ./test/iscsi_tgt/reset/reset.sh
run_test "iscsi_tgt_rpc_config" ./test/iscsi_tgt/rpc_config/rpc_config.sh $TEST_TYPE
run_test "iscsi_tgt_iscsi_lvol" ./test/iscsi_tgt/lvol/iscsi_lvol.sh
run_test "iscsi_tgt_fio" ./test/iscsi_tgt/fio/fio.sh
run_test "iscsi_tgt_qos" ./test/iscsi_tgt/qos/qos.sh

# IP Migration tests do not support network namespaces,
# they can only be run on posix sockets.
if [ "$TEST_TYPE" == "posix" ]; then
	run_test "iscsi_tgt_ip_migration" ./test/iscsi_tgt/ip_migration/ip_migration.sh
fi
run_test "iscsi_tgt_trace_record" ./test/iscsi_tgt/trace_record/trace_record.sh

if [ $RUN_NIGHTLY -eq 1 ]; then
	if [ $SPDK_TEST_PMDK -eq 1 ]; then
		run_test "iscsi_tgt_pmem" ./test/iscsi_tgt/pmem/iscsi_pmem.sh 4096 10
	fi
	run_test "iscsi_tgt_ext4test" ./test/iscsi_tgt/ext4test/ext4test.sh
	run_test "iscsi_tgt_digests" ./test/iscsi_tgt/digests/digests.sh
fi
if [ $SPDK_TEST_RBD -eq 1 ]; then
	# RBD tests do not support network namespaces,
	# they can only be run on posix sockets.
	if [ "$TEST_TYPE" == "posix" ]; then
		if ! hash ceph; then
			echo "ERROR: SPDK_TEST_RBD requested but no ceph installed!"
			false
		fi
		run_test "iscsi_tgt_rbd" ./test/iscsi_tgt/rbd/rbd.sh
	fi
fi

trap 'cleanup_veth_interfaces $TEST_TYPE; exit 1' SIGINT SIGTERM EXIT

if [ $SPDK_TEST_NVMF -eq 1 ]; then
	# NVMe-oF tests do not support network namespaces,
	# they can only be run on posix sockets.
	if [ "$TEST_TYPE" == "posix" ]; then
		# Test configure remote NVMe device from rpc and conf file
		run_test "iscsi_tgt_fio_remote_nvme" ./test/iscsi_tgt/nvme_remote/fio_remote_nvme.sh
	fi
fi

if [ $RUN_NIGHTLY -eq 1 ]; then
	if [ "$TEST_TYPE" == "posix" ]; then
		run_test "iscsi_tgt_fuzz" ./test/iscsi_tgt/fuzz/fuzz.sh
	fi
	run_test "iscsi_tgt_multiconnection" ./test/iscsi_tgt/multiconnection/multiconnection.sh
fi

if [ $SPDK_TEST_ISCSI_INITIATOR -eq 1 ]; then
	run_test "iscsi_tgt_initiator" ./test/iscsi_tgt/initiator/initiator.sh
	run_test "iscsi_tgt_bdev_io_wait" ./test/iscsi_tgt/bdev_io_wait/bdev_io_wait.sh
fi

cleanup_veth_interfaces $TEST_TYPE
trap - SIGINT SIGTERM EXIT
