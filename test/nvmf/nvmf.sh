#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/autotest_common.sh

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

export NVMF_APP="./app/nvmf_tgt/nvmf_tgt -i 0"

run_test test/nvmf/filesystem/filesystem.sh
run_test test/nvmf/discovery/discovery.sh
run_test test/nvmf/nvme_cli/nvme_cli.sh
run_test test/nvmf/lvol/nvmf_lvol.sh
run_test test/nvmf/shutdown/shutdown.sh

if [ $SPDK_TEST_NVML -eq 1 ]; then
	if [ $RUN_NIGHTLY -eq 1 ]; then
		run_test test/nvmf/pmem/nvmf_pmem.sh 30
		report_test_completion "nightly_nvmf_pmem"
	else
		run_test test/nvmf/pmem/nvmf_pmem.sh 10
		report_test_completion "nvmf_pmem"
	fi
fi

if [ $RUN_NIGHTLY -eq 1 ]; then
	run_test test/nvmf/multiconnection/multiconnection.sh
fi

timing_enter host

if [ $RUN_NIGHTLY -eq 1 ]; then
	# TODO: temporarily disabled - temperature AER doesn't fire on emulated controllers
	#run_test test/nvmf/host/aer.sh
	true
fi
run_test test/nvmf/host/bdevperf.sh
run_test test/nvmf/host/identify.sh
run_test test/nvmf/host/perf.sh
run_test test/nvmf/host/identify_kernel_nvmf.sh
run_test test/nvmf/host/fio.sh

timing_exit host
trap - SIGINT SIGTERM EXIT
kill_stub

# TODO: enable nvme device detachment for multi-process so that
#  we can use the stub for this test
run_test test/nvmf/rpc/rpc.sh
run_test test/nvmf/fio/fio.sh
revert_soft_roce

report_test_completion "nvmf"
timing_exit nvmf_tgt
