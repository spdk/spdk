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

run_test suite test/nvmf/target/filesystem.sh $TEST_ARGS
run_test suite test/nvmf/target/discovery.sh $TEST_ARGS
run_test suite test/nvmf/target/connect_disconnect.sh $TEST_ARGS
if [ $SPDK_TEST_NVME_CLI -eq 1 ]; then
	run_test suite test/nvmf/target/nvme_cli.sh $TEST_ARGS
fi
run_test suite test/nvmf/target/nvmf_lvol.sh $TEST_ARGS
#TODO: disabled due to intermittent failures. Need to triage.
# run_test suite test/nvmf/target/srq_overwhelm.sh $TEST_ARGS
run_test suite test/nvmf/target/nvmf_vhost.sh $TEST_ARGS
run_test suite test/nvmf/target/bdev_io_wait.sh $TEST_ARGS
run_test suite test/nvmf/target/create_transport.sh $TEST_ARGS
run_test suite test/nvmf/target/multitarget.sh $TEST_ARGS

if [ $RUN_NIGHTLY -eq 1 ]; then
	run_test suite test/nvmf/target/fuzz.sh $TEST_ARGS
	run_test suite test/nvmf/target/multiconnection.sh $TEST_ARGS
	run_test suite test/nvmf/target/initiator_timeout.sh $TEST_ARGS
fi

run_test suite test/nvmf/target/nmic.sh $TEST_ARGS
run_test suite test/nvmf/target/rpc.sh $TEST_ARGS
run_test suite test/nvmf/target/fio.sh $TEST_ARGS
run_test suite test/nvmf/target/shutdown.sh $TEST_ARGS
run_test suite test/nvmf/target/bdevio.sh $TEST_ARGS

timing_enter host

run_test suite test/nvmf/host/bdevperf.sh $TEST_ARGS
run_test suite test/nvmf/host/identify.sh $TEST_ARGS
run_test suite test/nvmf/host/perf.sh $TEST_ARGS

# TODO: disabled due to intermittent failures (RDMA_CM_EVENT_UNREACHABLE/ETIMEDOUT)
#run_test test/nvmf/host/identify_kernel_nvmf.sh $TEST_ARGS
run_test suite test/nvmf/host/aer.sh $TEST_ARGS
run_test suite test/nvmf/host/fio.sh $TEST_ARGS
run_test suite test/nvmf/host/target_disconnect.sh $TEST_ARGS

timing_exit host

trap - SIGINT SIGTERM EXIT
revert_soft_roce

report_test_completion "nvmf"
timing_exit nvmf_tgt
