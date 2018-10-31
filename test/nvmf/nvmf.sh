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

run_test suite test/nvmf/target/fuzz.sh
run_test suite test/nvmf/target/filesystem.sh
run_test suite test/nvmf/target/discovery.sh
run_test suite test/nvmf/target/connect_disconnect.sh
if [ $SPDK_TEST_NVME_CLI -eq 1 ]; then
	run_test suite test/nvmf/target/nvme_cli.sh
fi
run_test suite test/nvmf/target/nvmf_lvol.sh
#TODO: disabled due to intermittent failures. Need to triage.
# run_test suite test/nvmf/target/srq_overwhelm.sh
run_test suite test/nvmf/target/shutdown.sh
run_test suite test/nvmf/target/bdev_io_wait.sh
run_test suite test/nvmf/target/create_transport.sh

if [ $RUN_NIGHTLY -eq 1 ]; then
	run_test suite test/nvmf/target/multiconnection.sh
fi

run_test suite test/nvmf/target/nmic.sh
run_test suite test/nvmf/target/rpc.sh
run_test suite test/nvmf/target/fio.sh

timing_enter host

run_test suite test/nvmf/host/bdevperf.sh
run_test suite test/nvmf/host/identify.sh
run_test suite test/nvmf/host/perf.sh
# TODO: disabled due to intermittent failures (RDMA_CM_EVENT_UNREACHABLE/ETIMEDOUT)
#run_test test/nvmf/host/identify_kernel_nvmf.sh
run_test suite test/nvmf/host/aer.sh
if [ $SPDK_RUN_ASAN -eq 0 ]; then
    run_test suite test/nvmf/host/fio.sh
fi

timing_exit host

trap - SIGINT SIGTERM EXIT
revert_soft_roce

report_test_completion "nvmf"
timing_exit nvmf_tgt
