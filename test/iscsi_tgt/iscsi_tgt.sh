#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/autotest_common.sh

if [ ! $(uname -s) = Linux ]; then
	exit 0
fi

export TARGET_IP=127.0.0.1
export INITIATOR_IP=127.0.0.1

if [ $SPDK_TEST_VPP -eq 1 ]; then
	SPDK_VPP_DIR="/home/sys_sgci/vpp"
	export INITIATOR_IP=10.10.1.10
	VPP_CTL="$SPDK_VPP_DIR/build-root/install-vpp-native/vpp/bin/vppctl"
	cd $SPDK_VPP_DIR
	make run-release > /dev/null 2>&1 &
        sleep 10
	cd $rootdir
	$VPP_CTL tap connect tap0
        $VPP_CTL show int addr
	ip addr add 10.10.1.11/24 dev tap0
	ip link set tap0 up
	$VPP_CTL set interface state tapcli-0 up
	$VPP_CTL set interface ip address tapcli-0 $INITIATOR_IP/24
fi

source $rootdir/test/iscsi_tgt/common.sh

timing_enter iscsi_tgt

# ISCSI_TEST_CORE_MASK is the biggest core mask specified by
#  any of the iscsi_tgt tests.  Using this mask for the stub
#  ensures that if this mask spans CPU sockets, that we will
#  allocate memory from both sockets.  The stub will *not*
#  run anything on the extra cores (and will sleep on master
#  core 0) so there is no impact to the iscsi_tgt tests by
#  specifying the bigger core mask.
start_stub "-s 2048 -i 0 -m $ISCSI_TEST_CORE_MASK"
trap "kill_stub; exit 1" SIGINT SIGTERM EXIT

export ISCSI_APP="./app/iscsi_tgt/iscsi_tgt -i 0"

run_test ./test/iscsi_tgt/calsoft/calsoft.sh
run_test ./test/iscsi_tgt/filesystem/filesystem.sh
run_test ./test/iscsi_tgt/reset/reset.sh
run_test ./test/iscsi_tgt/rpc_config/rpc_config.sh
run_test ./test/iscsi_tgt/idle_migration/idle_migration.sh
run_test ./test/iscsi_tgt/lvol/iscsi_lvol.sh
run_test ./test/iscsi_tgt/fio/fio.sh

if [ $SPDK_TEST_NVML -eq 1 ]; then
	run_test ./test/iscsi_tgt/pmem/iscsi_pmem.sh 4096 10
fi

if [ $RUN_NIGHTLY -eq 1 ]; then
	run_test ./test/iscsi_tgt/ip_migration/ip_migration.sh
	run_test ./test/iscsi_tgt/ext4test/ext4test.sh
fi
if [ $SPDK_TEST_RBD -eq 1 ]; then
	run_test ./test/iscsi_tgt/rbd/rbd.sh
fi

trap - SIGINT SIGTERM EXIT
kill_stub

if [ $SPDK_TEST_NVMF -eq 1 ]; then
	# TODO: enable remote NVMe controllers with multi-process so that
	#  we can use the stub for this test
	# Test configure remote NVMe device from rpc
	run_test ./test/iscsi_tgt/nvme_remote/fio_remote_nvme.sh 0
	# Test configure remote NVMe device from conf file
	run_test ./test/iscsi_tgt/nvme_remote/fio_remote_nvme.sh 1
fi

timing_exit iscsi_tgt
