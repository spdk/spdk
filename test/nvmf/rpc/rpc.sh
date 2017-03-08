#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="python $rootdir/scripts/rpc.py"

set -e

if ! rdma_nic_available; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter rpc

# Start up the NVMf target in another process
$rootdir/app/nvmf_tgt/nvmf_tgt -c $testdir/../nvmf.conf &
pid=$!

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid ${RPC_PORT}

# set times for subsystem construct/delete
if [ $RUN_NIGHTLY -eq 1 ]; then
	times=50
else
	times=3
fi

# get all available nvme bdf info.
bdfs=$(lspci -mm -n | grep 0108 | tr -d '"' | awk -F " " '{print "0000:"$1}')

# do frequent add delete.
for i in `seq 1 $times`
do
	j=0
	for bdf in $bdfs; do
		let j=j+1
		$rpc_py construct_nvmf_subsystem Direct nqn.2016-06.io.spdk:cnode$j 'transport:RDMA traddr:192.168.100.8 trsvcid:4420' '' -p "$bdf"
	done

	n=$j
	for j in `seq 1 $n`
	do
		$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode$j
	done
done

trap - SIGINT SIGTERM EXIT

killprocess $pid
timing_exit rpc
