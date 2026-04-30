#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026, Oracle and/or its affiliates.
#
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../..")
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512
rpc_py="$rootdir/scripts/rpc.py"
subnqn=nqn.2016-06.io.spdk:cnode$$

function nvme_ctrlr_count() {
	local count=0
	local ctrlr

	for ctrlr in /sys/devices/virtual/nvme-fabrics/ctl/nvme*; do
		[[ -e "$ctrlr/subsysnqn" ]] || continue
		[[ "$(< "$ctrlr/subsysnqn")" == "$subnqn" ]] || continue
		((++count))
	done

	echo "$count"
}

nvmftestinit
nvmfappstart -m 0xF --wait-for-rpc

$rpc_py nvmf_set_config --dup-host-policy restrict_per_listener
$rpc_py framework_start_init
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py nvmf_create_subsystem "$subnqn" -a -s "$NVMF_SERIAL"
$rpc_py nvmf_subsystem_add_ns "$subnqn" Malloc0
$rpc_py nvmf_subsystem_add_listener "$subnqn" -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" \
	-s "$NVMF_PORT"
$rpc_py nvmf_subsystem_add_listener "$subnqn" -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" \
	-s "$NVMF_SECOND_PORT"

$NVME_CONNECT "${NVME_HOST[@]}" -t "$TEST_TRANSPORT" -n "$subnqn" -a "$NVMF_FIRST_TARGET_IP" \
	-s "$NVMF_PORT"
waitforserial "$NVMF_SERIAL"
waitforcondition '(( $(nvme_ctrlr_count) == 1 ))' 10

# Force the second connect attempt through the Linux host duplicate path check so the target policy
# rejects the duplicate host ID on the same listener.
NOT $NVME_CONNECT "${NVME_HOST[@]}" --duplicate-connect -t "$TEST_TRANSPORT" -n "$subnqn" \
	-a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"
waitforcondition '(( $(nvme_ctrlr_count) == 1 ))' 10

$NVME_CONNECT "${NVME_HOST[@]}" -t "$TEST_TRANSPORT" -n "$subnqn" -a "$NVMF_FIRST_TARGET_IP" \
	-s "$NVMF_SECOND_PORT"
waitforcondition '(( $(nvme_ctrlr_count) == 2 ))' 10

nvme disconnect -n "$subnqn"
waitforserial_disconnect "$NVMF_SERIAL"

trap - SIGINT SIGTERM EXIT

nvmftestfini
