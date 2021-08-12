#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"
loops=5

function jcount() {
	local filter=$1
	jq "$filter" | wc -l
}

function jsum() {
	local filter=$1
	jq "$filter" | awk '{s+=$1}END{print s}'
}

nvmftestinit
nvmfappstart -m 0xF

stats=$($rpc_py nvmf_get_stats)
# Expect 4 poll groups (from CPU mask) and no transports yet
[ "4" -eq $(jcount .poll_groups[].name <<< "$stats") ]
[ "null" == $(jq .poll_groups[0].transports[0] <<< "$stats") ]

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

stats=$($rpc_py nvmf_get_stats)
# Expect no QPs
[ "0" -eq $(jsum .poll_groups[].admin_qpairs <<< "$stats") ]
[ "0" -eq $(jsum .poll_groups[].io_qpairs <<< "$stats") ]
# Transport statistics is currently implemented for RDMA only
if [ 'rdma' == $TEST_TRANSPORT ]; then
	# Expect RDMA transport and some devices
	[ "1" -eq $(jcount .poll_groups[0].transports[].trtype <<< "$stats") ]
	transport_type=$(jq -r .poll_groups[0].transports[0].trtype <<< "$stats")
	[ "${transport_type,,}" == "${TEST_TRANSPORT,,}" ]
	[ "0" -lt $(jcount .poll_groups[0].transports[0].devices[].name <<< "$stats") ]
fi

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc1

# Disallow host NQN and make sure connect fails
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s $NVMF_SERIAL
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc1
$rpc_py nvmf_subsystem_allow_any_host -d nqn.2016-06.io.spdk:cnode1
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# This connect should fail - the host NQN is not allowed
NOT nvme connect -t $TEST_TRANSPORT -n nqn.2016-06.io.spdk:cnode1 -q nqn.2016-06.io.spdk:host1 -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

# Add the host NQN and verify that the connect succeeds
$rpc_py nvmf_subsystem_add_host nqn.2016-06.io.spdk:cnode1 nqn.2016-06.io.spdk:host1
nvme connect -t $TEST_TRANSPORT -n nqn.2016-06.io.spdk:cnode1 -q nqn.2016-06.io.spdk:host1 -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
waitforserial "$NVMF_SERIAL"
nvme disconnect -n nqn.2016-06.io.spdk:cnode1

# Remove the host and verify that the connect fails
$rpc_py nvmf_subsystem_remove_host nqn.2016-06.io.spdk:cnode1 nqn.2016-06.io.spdk:host1
NOT nvme connect -t $TEST_TRANSPORT -n nqn.2016-06.io.spdk:cnode1 -q nqn.2016-06.io.spdk:host1 -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

# Allow any host and verify that the connect succeeds
$rpc_py nvmf_subsystem_allow_any_host -e nqn.2016-06.io.spdk:cnode1
nvme connect -t $TEST_TRANSPORT -n nqn.2016-06.io.spdk:cnode1 -q nqn.2016-06.io.spdk:host1 -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
waitforserial "$NVMF_SERIAL"
nvme disconnect -n nqn.2016-06.io.spdk:cnode1

$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

# do frequent add delete of namespaces with different nsid.
for i in $(seq 1 $loops); do
	$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -s $NVMF_SERIAL
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc1 -n 5
	$rpc_py nvmf_subsystem_allow_any_host nqn.2016-06.io.spdk:cnode1
	nvme connect -t $TEST_TRANSPORT -n nqn.2016-06.io.spdk:cnode1 -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

	waitforserial "$NVMF_SERIAL"

	nvme disconnect -n nqn.2016-06.io.spdk:cnode1

	$rpc_py nvmf_subsystem_remove_ns nqn.2016-06.io.spdk:cnode1 5
	$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

done

# do frequent add delete.
for i in $(seq 1 $loops); do
	$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -s $NVMF_SERIAL
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc1
	$rpc_py nvmf_subsystem_allow_any_host nqn.2016-06.io.spdk:cnode1

	$rpc_py nvmf_subsystem_remove_ns nqn.2016-06.io.spdk:cnode1 1

	$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1
done

stats=$($rpc_py nvmf_get_stats)
# Expect some admin and IO qpairs
[ "0" -lt $(jsum .poll_groups[].admin_qpairs <<< "$stats") ]
[ "0" -lt $(jsum .poll_groups[].io_qpairs <<< "$stats") ]
# Transport statistics is currently implemented for RDMA only
if [ 'rdma' == $TEST_TRANSPORT ]; then
	# Expect non-zero completions and request latencies accumulated
	[ "0" -lt $(jsum .poll_groups[].transports[].devices[].completions <<< "$stats") ]
	[ "0" -lt $(jsum .poll_groups[].transports[].devices[].request_latency <<< "$stats") ]
fi

trap - SIGINT SIGTERM EXIT

nvmftestfini
