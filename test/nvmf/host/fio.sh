#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="python $rootdir/scripts/rpc.py"

set -e

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

if [ ! -d /usr/src/fio ]; then
	echo "FIO not available"
	exit 0
fi

timing_enter fio
timing_enter start_nvmf_tgt

$NVMF_APP -c $testdir/../nvmf.conf &
nvmfpid=$!

trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
timing_exit start_nvmf_tgt

bdevs="$bdevs $($rpc_py construct_malloc_bdev 64 512)"
$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" '' -a -s SPDK00000000000001 -n "$bdevs"

PLUGIN_DIR=$rootdir/examples/nvme/fio_plugin
LD_PRELOAD=$PLUGIN_DIR/fio_plugin

bdfs=$(lspci -mm -n | grep 0108 | tr -d '"' | awk -F " " '{print "0000:"$1}')
#Configure nvme devices with nvmf lvol_bdev backend
if [ -n "$bdfs" ]; then
        $rpc_py construct_nvme_bdev -b Nvme0 -t PCIe -a $(echo $bdfs | awk '{ print $1 }')
        ls_guid=$($rpc_py construct_lvol_store Nvme0n1 lvs_0)
	lb_guid=$($rpc_py construct_lvol_bdev -u $ls_guid  lbd_0 16000)
        $rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk-cnode2 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" "" -a -s SPDK00000000000002 -n "$lb_guid"
	#Create lvol bdev for nested lvol stores
	ls_nested_guid=$($rpc_py construct_lvol_store $lb_guid lvs_n_0)
	lb_nested_guid=$($rpc_py construct_lvol_bdev -u $ls_nested_guid lbd_nest_0 2000)
	$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk-cnode3 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:4420" "" -a -s SPDK00000000000003 -n "$lb_nested_guid"
	#Test fio_plugin connect to nvmf subnqn with nvmf lvol and malloc backend
	/usr/src/fio/fio $PLUGIN_DIR/example_config.fio --filename="trtype=RDMA adrfam=IPv4 \
	traddr=$NVMF_FIRST_TARGET_IP subnqn=nqn.2016-06.io.spdk:cnode1 trsvcid=4420 ns=1" --filename="trtype=RDMA adrfam=IPv4 \
	traddr=$NVMF_FIRST_TARGET_IP subnqn=nqn.2016-06.io.spdk-cnode2 trsvcid=4420 ns=1" --filename="trtype=RDMA adrfam=IPv4 \
	traddr=$NVMF_FIRST_TARGET_IP subnqn=nqn.2016-06.io.spdk-cnode3 trsvcid=4420 ns=1"
fi

if [ -z "$bdfs" ]; then
	/usr/src/fio/fio $PLUGIN_DIR/example_config.fio --filename="trtype=RDMA adrfam=IPv4 \
	traddr=$NVMF_FIRST_TARGET_IP subnqn=nqn.2016-06.io.spdk:cnode1 trsvcid=4420 ns=1"
fi

sync
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

#Delete subsystems,lvol_bdev and destory lvol_store.
if [ -n "$bdfs" ]; then
        $rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk-cnode2
	$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk-cnode3
        $rpc_py delete_bdev  "$lb_nested_guid"
        $rpc_py destroy_lvol_store -l lvs_n_0
	$rpc_py delete_bdev  "$lb_guid"
	$rpc_py destroy_lvol_store -l lvs_0
        $rpc_py delete_bdev "Nvme0n1"
fi

trap - SIGINT SIGTERM EXIT

killprocess $nvmfpid
timing_exit fio
