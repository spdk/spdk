#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

if [ ! $(uname -s) = Linux ]; then
	exit 0
fi

source $rootdir/test/nvmf/common.sh

timing_enter nvmf_tgt

# NVMF_TEST_CORE_MASK is the biggest core mask specified by
#  any of the nvmf_tgt tests.  Using this mask for the stub
#  ensures that if this mask spans CPU sockets, that we will
#  allocate memory from both sockets.  The stub will *not*
#  run anything on the extra cores (and will sleep on master
#  core 0) so there is no impact to the nvmf_tgt tests by
#  specifying the bigger core mask.
start_stub "-s 2048 -i 0 -m $NVMF_TEST_CORE_MASK"
trap "kill_stub; exit 1" SIGINT SIGTERM EXIT

export NVMF_APP_SHM_ID="0"
export NVMF_APP="./app/nvmf_tgt/nvmf_tgt -i $NVMF_APP_SHM_ID -e 0xFFFF"

run_test suite test/nvmf/filesystem/filesystem.sh
run_test suite test/nvmf/discovery/discovery.sh
if [ $SPDK_TEST_NVME_CLI -eq 1 ]; then
	run_test suite test/nvmf/nvme_cli/nvme_cli.sh
fi
run_test suite test/nvmf/lvol/nvmf_lvol.sh
run_test suite test/nvmf/shutdown/shutdown.sh
run_test suite test/nvmf/bdev_io_wait/bdev_io_wait.sh
run_test suite test/nvmf/create_transport/create_transport.sh

if [ $RUN_NIGHTLY -eq 1 ]; then
	run_test suite test/nvmf/multiconnection/multiconnection.sh
fi

timing_enter host

run_test suite test/nvmf/host/bdevperf.sh
run_test suite test/nvmf/host/identify.sh
run_test suite test/nvmf/host/perf.sh
# TODO: disabled due to intermittent failures (RDMA_CM_EVENT_UNREACHABLE/ETIMEDOUT)
#run_test test/nvmf/host/identify_kernel_nvmf.sh
run_test suite test/nvmf/host/aer.sh
run_test suite test/nvmf/host/fio.sh

run_test suite test/nvmf/nmic/nmic.sh

timing_exit host
trap - SIGINT SIGTERM EXIT
kill_stub

# TODO: enable nvme device detachment for multi-process so that
#  we can use the stub for this test
run_test suite test/nvmf/rpc/rpc.sh
run_test suite test/nvmf/fio/fio.sh
revert_soft_roce

report_test_completion "nvmf"
timing_exit nvmf_tgt
