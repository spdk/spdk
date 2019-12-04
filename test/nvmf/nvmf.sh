#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

if [ ! $(uname -s) = Linux ]; then
	exit 0
fi

source $rootdir/test/nvmf/common.sh

timing_enter nvmf_tgt

trap "exit 1" SIGINT SIGTERM EXIT

TEST_ARGS=( "$@" )

run_test suite "nvmf_filesystem" test/nvmf/target/filesystem.sh "${TEST_ARGS[@]}"
run_test suite "nvmf_discovery" test/nvmf/target/discovery.sh "${TEST_ARGS[@]}"
run_test suite "nvmf_connect_disconnect" test/nvmf/target/connect_disconnect.sh "${TEST_ARGS[@]}"
if [ $SPDK_TEST_NVME_CLI -eq 1 ]; then
	run_test suite "nvmf_nvme_cli" test/nvmf/target/nvme_cli.sh "${TEST_ARGS[@]}"
fi
run_test suite "nvmf_lvol" test/nvmf/target/nvmf_lvol.sh "${TEST_ARGS[@]}"
#TODO: disabled due to intermittent failures. Need to triage.
# run_test suite "nvmf_srq_overwhelm" test/nvmf/target/srq_overwhelm.sh $TEST_ARGS
run_test suite "nvmf_vhost" test/nvmf/target/nvmf_vhost.sh "${TEST_ARGS[@]}"
run_test suite "nvmf_bdev_io_wait" test/nvmf/target/bdev_io_wait.sh "${TEST_ARGS[@]}"
run_test suite "nvmf_create_transport." test/nvmf/target/create_transport.sh "${TEST_ARGS[@]}"
run_test suite "nvmf_multitarget" test/nvmf/target/multitarget.sh "${TEST_ARGS[@]}"

if [ $RUN_NIGHTLY -eq 1 ]; then
	run_test suite "nvmf_fuzz" test/nvmf/target/fuzz.sh "${TEST_ARGS[@]}"
	run_test suite "nvmf_multiconnection" test/nvmf/target/multiconnection.sh "${TEST_ARGS[@]}"
	run_test suite "nvmf_initiator_timeout" test/nvmf/target/initiator_timeout.sh "${TEST_ARGS[@]}"
fi

run_test suite "nvmf_nmic" test/nvmf/target/nmic.sh "${TEST_ARGS[@]}"
run_test suite "nvmf_rpc" test/nvmf/target/rpc.sh "${TEST_ARGS[@]}"
run_test suite "nvmf_fio" test/nvmf/target/fio.sh "${TEST_ARGS[@]}"
run_test suite "nvmf_shutdown" test/nvmf/target/shutdown.sh "${TEST_ARGS[@]}"
run_test suite "nvmf_bdevio" test/nvmf/target/bdevio.sh "${TEST_ARGS[@]}"

timing_enter host

run_test suite "nvmf_bdevperf" test/nvmf/host/bdevperf.sh "${TEST_ARGS[@]}"
run_test suite "nvmf_identify" test/nvmf/host/identify.sh "${TEST_ARGS[@]}"
run_test suite "nvmf_perf" test/nvmf/host/perf.sh "${TEST_ARGS[@]}"

# TODO: disabled due to intermittent failures (RDMA_CM_EVENT_UNREACHABLE/ETIMEDOUT)
#run_test test/nvmf/host/identify_kernel_nvmf.sh $TEST_ARGS
run_test suite "nvmf_aer" test/nvmf/host/aer.sh "${TEST_ARGS[@]}"
run_test suite "nvmf_fio" test/nvmf/host/fio.sh "${TEST_ARGS[@]}"
run_test suite "nvmf_target_disconnect" test/nvmf/host/target_disconnect.sh "${TEST_ARGS[@]}"

timing_exit host

trap - SIGINT SIGTERM EXIT
revert_soft_roce

report_test_completion "nvmf"
timing_exit nvmf_tgt
