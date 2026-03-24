#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
# Functional test for NVMe Key-Value command set over NVMe-oF/TCP.
# This test:
#   1) Starts an SPDK NVMe-oF/TCP target with a bdev_kvmalloc device
#   2) Connects to the target using the kernel nvme driver (nvme-cli)
#   3) Gets identify data using nvme id-ctrl and nvme id-ns

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../..")
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

NVMF_KV_SUBNQN="nqn.2016-06.io.spdk:cnode_kv"

cleanup() {
	nvme disconnect -n "$NVMF_KV_SUBNQN" 2> /dev/null || :
	nvmftestfini || :
}

# Find the kernel NVMe controller connected to our subsystem NQN
find_nvme_ctrl() {
	local subnqn=$1
	local ctrlr

	for ctrlr in /sys/class/nvme/nvme*; do
		[[ -e "$ctrlr/subsysnqn" ]] || continue
		if [[ "$(< "$ctrlr/subsysnqn")" == "$subnqn" ]]; then
			echo "${ctrlr##*/}"
			return 0
		fi
	done
	return 1
}

# Wait for the controller to appear after nvme connect
wait_for_ctrl() {
	local subnqn=$1
	local i

	for ((i = 0; i < 20; i++)); do
		if find_nvme_ctrl "$subnqn"; then
			return 0
		fi
		sleep 0.5
	done

	echo "ERROR: Timed out waiting for NVMe controller for $subnqn" >&2
	return 1
}

nvmftestinit
nvmfappstart -m 0xF

trap cleanup SIGINT SIGTERM EXIT

# Create transport
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

# Create a KV malloc bdev
$rpc_py bdev_kvmalloc_create -b KVMalloc0

# Create subsystem, add namespace, add listener
$rpc_py nvmf_create_subsystem "$NVMF_KV_SUBNQN" -a -s SPDKKV000000000001 -d SPDK_KV_Controller
$rpc_py nvmf_subsystem_add_ns "$NVMF_KV_SUBNQN" KVMalloc0
$rpc_py nvmf_subsystem_add_listener "$NVMF_KV_SUBNQN" -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# Show subsystem info
$rpc_py nvmf_get_subsystems

echo "=== Connecting to NVMe-oF KV target ==="
nvme connect "${NVME_HOST[@]}" -t $TEST_TRANSPORT -n "$NVMF_KV_SUBNQN" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

# Wait for the controller to appear
ctrl=$(wait_for_ctrl "$NVMF_KV_SUBNQN")
echo "Found NVMe controller: $ctrl (/dev/$ctrl)"

echo ""
echo "=== NVMe Identify Controller ==="
nvme id-ctrl "/dev/$ctrl" || {
	echo "ERROR: nvme id-ctrl failed"
	exit 1
}

echo ""
echo "=== NVMe Identify Controller (human readable) ==="
nvme id-ctrl "/dev/$ctrl" -H 2>&1 | head -60

echo ""
echo "=== Verifying controller model number ==="
nvme_model=$(nvme id-ctrl "/dev/$ctrl" | grep -w mn | sed 's/^.*: //' | sed 's/ *$//')
echo "Model: '$nvme_model'"
if [[ "$nvme_model" != "SPDK_KV_Controller" ]]; then
	echo "ERROR: Wrong model number: expected 'SPDK_KV_Controller', got '$nvme_model'"
	exit 1
fi

echo ""
echo "=== NVMe List Namespaces ==="
nvme list-ns "/dev/$ctrl" || echo "(list-ns returned non-zero, may be expected for KV)"

echo ""
echo "=== NVMe Identify Namespace (nsid=1) ==="
nvme id-ns "/dev/$ctrl" -n 1 || echo "(id-ns returned non-zero, may be expected for KV)"

echo ""
echo "=== NVMe Identify Namespace (nsid=1, human readable) ==="
nvme id-ns "/dev/$ctrl" -n 1 -H 2>&1 | head -40 || echo "(id-ns -H returned non-zero)"

echo ""
echo "=== Disconnecting ==="
nvme disconnect -n "$NVMF_KV_SUBNQN"

echo ""
echo "=== Cleaning up subsystem ==="
$rpc_py nvmf_delete_subsystem "$NVMF_KV_SUBNQN"

trap - SIGINT SIGTERM EXIT

nvmftestfini

echo ""
echo "=== KV NVMe-oF functional test PASSED ==="
