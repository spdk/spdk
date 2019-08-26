#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

function build_nvmf_example_args()
{
        if [ $SPDK_RUN_NON_ROOT -eq 1 ]; then
                echo "sudo -u $(logname) ./examples/nvmf/nvmf/nvmf -i $NVMF_APP_SHM_ID"
        else
                echo "./examples/nvmf/nvmf/nvmf -i $NVMF_APP_SHM_ID"
        fi
}

NVMF_EXAMPLE="$(build_nvmf_example_args)"

function nvmfexamplestart()
{
        timing_enter start_nvmf_example
        $NVMF_EXAMPLE $1
        nvmfpid=$!
        timing_exit start_nvmf_example
}

timing_enter nvmf_example_test
nvmftestinit
nvmfexamplestart "-m 0xF"

nvmftestfini
timing_exit nvmf_example_test
