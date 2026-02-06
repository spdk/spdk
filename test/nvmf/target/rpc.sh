#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

loops=5
subnqn=nqn.2016-06.io.spdk:cnode$$

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
(($(jcount '.poll_groups[].name' <<< "$stats") == 4))
[[ $(jq '.poll_groups[0].transports[0]' <<< "$stats") == null ]]

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

stats=$($rpc_py nvmf_get_stats)
# Expect no QPs
(($(jsum '.poll_groups[].admin_qpairs' <<< "$stats") == 0))
(($(jsum '.poll_groups[].io_qpairs' <<< "$stats") == 0))
# Transport statistics is currently implemented for RDMA only
if [ 'rdma' == $TEST_TRANSPORT ]; then
	# Expect RDMA transport and some devices
	(($(jcount '.poll_groups[0].transports[].trtype' <<< "$stats") == 1))
	transport_type=$(jq -r '.poll_groups[0].transports[0].trtype' <<< "$stats")
	[[ "${transport_type,,}" == "${TEST_TRANSPORT,,}" ]]
	(($(jcount '.poll_groups[0].transports[0].devices[].name' <<< "$stats") > 0))
fi

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc1

# Disallow host NQN and make sure connect fails
$rpc_py nvmf_create_subsystem $subnqn -a -s $NVMF_SERIAL
$rpc_py nvmf_subsystem_add_ns $subnqn Malloc1
$rpc_py nvmf_subsystem_allow_any_host --no-allow-any-host $subnqn
$rpc_py nvmf_subsystem_add_listener $subnqn -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# This connect should fail - the host NQN is not allowed
NOT $NVME_CONNECT "${NVME_HOST[@]}" -t $TEST_TRANSPORT -n $subnqn -q "$NVME_HOSTNQN" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

# Add the host NQN and verify that the connect succeeds
$rpc_py nvmf_subsystem_add_host $subnqn "$NVME_HOSTNQN"
$NVME_CONNECT "${NVME_HOST[@]}" -t $TEST_TRANSPORT -n $subnqn -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
waitforserial "$NVMF_SERIAL"
nvme disconnect -n $subnqn
waitforserial_disconnect "$NVMF_SERIAL"

# Remove the host and verify that the connect fails
$rpc_py nvmf_subsystem_remove_host $subnqn "$NVME_HOSTNQN"
NOT $NVME_CONNECT "${NVME_HOST[@]}" -t $TEST_TRANSPORT -n $subnqn -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

# Allow any host and verify that the connect succeeds
$rpc_py nvmf_subsystem_allow_any_host --allow-any-host $subnqn
$NVME_CONNECT "${NVME_HOST[@]}" -t $TEST_TRANSPORT -n $subnqn -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
waitforserial "$NVMF_SERIAL"
nvme disconnect -n $subnqn
waitforserial_disconnect "$NVMF_SERIAL"

$rpc_py nvmf_delete_subsystem $subnqn

# do frequent add delete of namespaces with different nsid.
for i in $(seq 1 $loops); do
	$rpc_py nvmf_create_subsystem $subnqn -s $NVMF_SERIAL
	$rpc_py nvmf_subsystem_add_listener $subnqn -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
	$rpc_py nvmf_subsystem_add_ns $subnqn Malloc1 -n 5
	$rpc_py nvmf_subsystem_allow_any_host --allow-any-host $subnqn
	$NVME_CONNECT "${NVME_HOST[@]}" -t $TEST_TRANSPORT -n $subnqn -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

	waitforserial "$NVMF_SERIAL"

	nvme disconnect -n $subnqn
	waitforserial_disconnect "$NVMF_SERIAL"

	$rpc_py nvmf_subsystem_remove_ns $subnqn 5
	$rpc_py nvmf_delete_subsystem $subnqn

done

# do frequent add delete.
for i in $(seq 1 $loops); do
	$rpc_py nvmf_create_subsystem $subnqn -s $NVMF_SERIAL
	$rpc_py nvmf_subsystem_add_listener $subnqn -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
	$rpc_py nvmf_subsystem_add_ns $subnqn Malloc1
	$rpc_py nvmf_subsystem_allow_any_host --allow-any-host $subnqn

	$rpc_py nvmf_subsystem_remove_ns $subnqn 1

	$rpc_py nvmf_delete_subsystem $subnqn
done

stats=$($rpc_py nvmf_get_stats)
# Expect some admin and IO qpairs
(($(jsum '.poll_groups[].admin_qpairs' <<< "$stats") > 0))
(($(jsum '.poll_groups[].io_qpairs' <<< "$stats") > 0))
# Transport statistics is currently implemented for RDMA only
if [ 'rdma' == $TEST_TRANSPORT ]; then
	# Expect non-zero completions and request latencies accumulated
	(($(jsum '.poll_groups[].transports[].devices[].completions' <<< "$stats") > 0))
	(($(jsum '.poll_groups[].transports[].devices[].request_latency' <<< "$stats") > 0))
fi

trap - SIGINT SIGTERM EXIT

nvmftestfini
