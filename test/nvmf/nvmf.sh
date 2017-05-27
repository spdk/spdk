#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/autotest_common.sh

if [ ! $(uname -s) = Linux ]; then
	exit 0
fi

source $rootdir/test/nvmf/common.sh

timing_enter nvmf_tgt

run_test test/nvmf/fio/fio.sh
run_test test/nvmf/filesystem/filesystem.sh
run_test test/nvmf/discovery/discovery.sh
run_test test/nvmf/nvme_cli/nvme_cli.sh
run_test test/nvmf/shutdown/shutdown.sh
run_test test/nvmf/rpc/rpc.sh

if [ $RUN_NIGHTLY -eq 1 ]; then
	run_test test/nvmf/multiconnection/multiconnection.sh
fi

timing_enter host

if [ $RUN_NIGHTLY -eq 1 ]; then
	run_test test/nvmf/host/aer.sh
fi
run_test test/nvmf/host/identify.sh
run_test test/nvmf/host/perf.sh
run_test test/nvmf/host/identify_kernel_nvmf.sh
run_test test/nvmf/host/fio.sh

timing_exit host

timing_exit nvmf_tgt
