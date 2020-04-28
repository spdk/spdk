#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

# For the time being this script is just menat to confirm the basic functionality of the
# multitarget RPCs as the in-tree applications don't support multi-target functionality.
rpc_py="$rootdir/test/nvmf/target/multitarget_rpc.py"

nvmftestinit
nvmfappstart -m 0xF

trap 'process_shm --id $NVMF_APP_SHM_ID; nvmftestfini $1; exit 1' SIGINT SIGTERM EXIT

# Target application should start with a single target.
if [ "$($rpc_py nvmf_get_targets | jq 'length')" != "1" ]; then
	echo "SPDK application did not start with the proper number of targets." && false
fi

$rpc_py nvmf_create_target -n nvmf_tgt_1 -s 32
$rpc_py nvmf_create_target -n nvmf_tgt_2 -s 32

if [ "$($rpc_py nvmf_get_targets | jq 'length')" != "3" ]; then
	echo "nvmf_create_target RPC didn't properly create targets." && false
fi

$rpc_py nvmf_delete_target -n nvmf_tgt_1
$rpc_py nvmf_delete_target -n nvmf_tgt_2

if [ "$($rpc_py nvmf_get_targets | jq 'length')" != "1" ]; then
	echo "nvmf_delete_target RPC didn't properly destroy targets." && false
fi

trap - SIGINT SIGTERM EXIT

nvmftestfini
