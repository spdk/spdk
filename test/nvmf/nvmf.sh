#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

if [ ! $(uname -s) = Linux ]; then
	exit 0
fi

source $rootdir/test/nvmf/common.sh

trap "exit 1" SIGINT SIGTERM EXIT

TEST_ARGS=("$@")

run_test "nvmf_example" test/nvmf/target/nvmf_example.sh "${TEST_ARGS[@]}"
run_test "nvmf_filesystem" test/nvmf/target/filesystem.sh "${TEST_ARGS[@]}"
run_test "nvmf_discovery" test/nvmf/target/discovery.sh "${TEST_ARGS[@]}"
run_test "nvmf_connect_disconnect" test/nvmf/target/connect_disconnect.sh "${TEST_ARGS[@]}"
run_test "nvmf_host_management" test/nvmf/target/host_management.sh "${TEST_ARGS[@]}"
if [ $SPDK_TEST_NVME_CLI -eq 1 ]; then
	run_test "nvmf_nvme_cli" test/nvmf/target/nvme_cli.sh "${TEST_ARGS[@]}"
fi
run_test "nvmf_lvol" test/nvmf/target/nvmf_lvol.sh "${TEST_ARGS[@]}"
run_test "nvmf_vhost" test/nvmf/target/nvmf_vhost.sh "${TEST_ARGS[@]}"
run_test "nvmf_bdev_io_wait" test/nvmf/target/bdev_io_wait.sh "${TEST_ARGS[@]}"
run_test "nvmf_create_transport." test/nvmf/target/create_transport.sh "${TEST_ARGS[@]}"
run_test "nvmf_multitarget" test/nvmf/target/multitarget.sh "${TEST_ARGS[@]}"

if [ $RUN_NIGHTLY -eq 1 ]; then
	run_test "nvmf_fuzz" test/nvmf/target/fuzz.sh "${TEST_ARGS[@]}"
	run_test "nvmf_multiconnection" test/nvmf/target/multiconnection.sh "${TEST_ARGS[@]}"
	run_test "nvmf_initiator_timeout" test/nvmf/target/initiator_timeout.sh "${TEST_ARGS[@]}"
fi

run_test "nvmf_nmic" test/nvmf/target/nmic.sh "${TEST_ARGS[@]}"
run_test "nvmf_rpc" test/nvmf/target/rpc.sh "${TEST_ARGS[@]}"
run_test "nvmf_fio" test/nvmf/target/fio.sh "${TEST_ARGS[@]}"
run_test "nvmf_bdevio" test/nvmf/target/bdevio.sh "${TEST_ARGS[@]}"
run_test "nvmf_invalid" test/nvmf/target/invalid.sh "${TEST_ARGS[@]}"
run_test "nvmf_abort" test/nvmf/target/abort.sh "${TEST_ARGS[@]}"

if ! check_ip_is_soft_roce $NVMF_FIRST_TARGET_IP; then
	# Soft-RoCE will return invalid values in the WC field after a qp has been
	# destroyed which lead to NULL pointer references not seen in real hardware.
	run_test "nvmf_shutdown" test/nvmf/target/shutdown.sh "${TEST_ARGS[@]}"
	#TODO: disabled due to intermittent failures. Need to triage.
	# run_test "nvmf_srq_overwhelm" test/nvmf/target/srq_overwhelm.sh $TEST_ARGS
fi

run_test "nvmf_multipath" test/nvmf/target/multipath.sh "${TEST_ARGS[@]}"

timing_enter host

run_test "nvmf_identify" test/nvmf/host/identify.sh "${TEST_ARGS[@]}"
run_test "nvmf_perf" test/nvmf/host/perf.sh "${TEST_ARGS[@]}"
run_test "nvmf_multipath" test/nvmf/host/multipath.sh "${TEST_ARGS[@]}"
run_test "nvmf_multicontroller" test/nvmf/host/multicontroller.sh "${TEST_ARGS[@]}"

# TODO: disabled due to intermittent failures (RDMA_CM_EVENT_UNREACHABLE/ETIMEDOUT)
#run_test test/nvmf/host/identify_kernel_nvmf.sh $TEST_ARGS
run_test "nvmf_aer" test/nvmf/host/aer.sh "${TEST_ARGS[@]}"
run_test "nvmf_fio" test/nvmf/host/fio.sh "${TEST_ARGS[@]}"

# There is an intermittent error relating to those tests and Soft-RoCE.
# Skip those tests if we are using rxe.
if ! check_ip_is_soft_roce $NVMF_FIRST_TARGET_IP; then
	# GitHub issue #1165
	run_test "nvmf_bdevperf" test/nvmf/host/bdevperf.sh "${TEST_ARGS[@]}"
	# GitHub issue #1043
	run_test "nvmf_target_disconnect" test/nvmf/host/target_disconnect.sh "${TEST_ARGS[@]}"
fi

timing_exit host

trap - SIGINT SIGTERM EXIT
revert_soft_roce
