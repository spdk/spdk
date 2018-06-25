#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

if [ ! $(uname -s) = Linux ]; then
	exit 0
fi

source $rootdir/test/iscsi_tgt/common.sh

timing_enter iscsi_tgt

# $1 = test type (posix/vpp)
if [ "$1" == "posix" ] || [ "$1" == "vpp" ]; then
	TEST_TYPE=$1
else
	echo "No iSCSI test type specified"
	exit 1
fi

# Network configuration
create_veth_interfaces $TEST_TYPE

trap "cleanup_veth_interfaces $TEST_TYPE; exit 1" SIGINT SIGTERM EXIT

run_test suite ./test/iscsi_tgt/sock/sock.sh
run_test suite ./test/iscsi_tgt/fio/fio.sh
if [ "$TEST_TYPE" == "posix" ]; then
	# TODO: fix tests below to work with VPP
	run_test suite ./test/iscsi_tgt/rpc_config/rpc_config.sh $TEST_TYPE
	run_test suite ./test/iscsi_tgt/calsoft/calsoft.sh
fi
run_test suite ./test/iscsi_tgt/filesystem/filesystem.sh
run_test suite ./test/iscsi_tgt/reset/reset.sh
run_test suite ./test/iscsi_tgt/lvol/iscsi_lvol.sh
run_test suite ./test/iscsi_tgt/qos/qos.sh
if [ "$TEST_TYPE" == "posix" ]; then
	run_test suite ./test/iscsi_tgt/ip_migration/ip_migration.sh
	run_test suite ./test/iscsi_tgt/trace_record/trace_record.sh
fi

if [ $RUN_NIGHTLY -eq 1 ]; then
	if [ $SPDK_TEST_PMDK -eq 1 ]; then
		run_test suite ./test/iscsi_tgt/pmem/iscsi_pmem.sh 4096 10
	fi
	run_test suite ./test/iscsi_tgt/ext4test/ext4test.sh
	run_test suite ./test/iscsi_tgt/digests/digests.sh
fi
if [ $SPDK_TEST_RBD -eq 1 ]; then
	# RBD tests do not support network namespaces,
	# they can only be run on posix sockets.
	if [ "$TEST_TYPE" == "posix" ]; then
		run_test suite ./test/iscsi_tgt/rbd/rbd.sh
	fi
fi

trap "cleanup_veth_interfaces $TEST_TYPE; exit 1" SIGINT SIGTERM EXIT

if [ $SPDK_TEST_NVMF -eq 1 ]; then
	# NVMe-oF tests do not support network namespaces,
	# they can only be run on posix sockets.
	if [ "$TEST_TYPE" == "posix" ]; then
		# Test configure remote NVMe device from rpc and conf file
		run_test suite ./test/iscsi_tgt/nvme_remote/fio_remote_nvme.sh
	fi
fi

if [ $RUN_NIGHTLY -eq 1 ]; then
	run_test suite ./test/iscsi_tgt/multiconnection/multiconnection.sh
fi

if [ $SPDK_TEST_ISCSI_INITIATOR -eq 1 ]; then
	run_test suite ./test/iscsi_tgt/bdev_io_wait/bdev_io_wait.sh
	# TODO: fix initiator test for VPP
	if [ "$TEST_TYPE" == "posix" ]; then
		run_test suite ./test/iscsi_tgt/initiator/initiator.sh
	fi
fi

cleanup_veth_interfaces $TEST_TYPE
trap - SIGINT SIGTERM EXIT
timing_exit iscsi_tgt
