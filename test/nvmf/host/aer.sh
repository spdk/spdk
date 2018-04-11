#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="python $rootdir/scripts/rpc.py"

set -e

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

timing_enter aer
timing_enter start_nvmf_tgt

$NVMF_APP -s 512 -c $testdir/../nvmf.conf &
nvmfpid=$!

trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $nvmfpid
timing_exit start_nvmf_tgt

modprobe -v nvme-rdma

$rpc_py construct_malloc_bdev 64 512 --name Malloc0
$rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 "trtype:RDMA traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" '' -a -s SPDK00000000000001 -n Malloc0

# TODO: this aer test tries to invoke an AER completion by setting the temperature
#threshold to a very low value.  This does not work with emulated controllers
#though so currently the test is disabled.

#$rootdir/test/nvme/aer/aer -r "\
#        trtype:RDMA \
#        adrfam:IPv4 \
#        traddr:$NVMF_FIRST_TARGET_IP \
#        trsvcid:$NVMF_PORT \
#        subnqn:nqn.2014-08.org.nvmexpress.discovery"

# Namespace Attribute Notice Tests
$rootdir/test/nvme/aer/aer -r "\
        trtype:RDMA \
        adrfam:IPv4 \
        traddr:$NVMF_FIRST_TARGET_IP \
        trsvcid:$NVMF_PORT \
        subnqn:nqn.2016-06.io.spdk:cnode1" -n 2 &
aerpid=$!

# Waiting for aer start to work
sleep 5

# Add a new namespace
$rpc_py construct_malloc_bdev 64 4096 --name Malloc1
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc1 -n 2

wait $aerpid

$rpc_py delete_bdev Malloc0
$rpc_py delete_bdev Malloc1
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1

trap - SIGINT SIGTERM EXIT

nvmfcleanup
killprocess $nvmfpid
timing_exit aer
