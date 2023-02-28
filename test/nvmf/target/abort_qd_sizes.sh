#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../../")

# Set defaults we want to work with
set -- "--transport=tcp" "--iso" "$@"

source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

shopt -s nullglob

rabort() {
	local trtype=$1
	local adrfam=$2
	local traddr=$3
	local trsvcid=$4
	local subnqn=$5

	local qds qd
	local target r

	qds=(4 24 64)

	for r in trtype adrfam traddr trsvcid subnqn; do
		target=${target:+$target }$r:${!r}
	done

	for qd in "${qds[@]}"; do
		"$SPDK_EXAMPLE_DIR/abort" \
			-q "$qd" \
			-w rw \
			-M 50 \
			-o 4096 \
			-r "$target"
	done
}

spdk_target() {
	local name=spdk_target
	local subnqn=nqn.2016-06.io.spdk:$name

	rpc_cmd bdev_nvme_attach_controller -t pcie -a "$nvme" -b "$name"

	rpc_cmd nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
	rpc_cmd nvmf_create_subsystem "$subnqn" -a -s "$NVMF_SERIAL"
	rpc_cmd nvmf_subsystem_add_ns "$subnqn" "${name}n1"
	rpc_cmd nvmf_subsystem_add_listener "$subnqn" -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT"

	rabort "$TEST_TRANSPORT" IPv4 "$NVMF_FIRST_TARGET_IP" "$NVMF_PORT" "$subnqn"

	rpc_cmd nvmf_delete_subsystem "$subnqn"
	rpc_cmd bdev_nvme_detach_controller "$name"

	# Make sure we fully detached from the ctrl as vfio-pci won't be able to release the
	# device otherwise - we can either wait a bit or simply kill the app. Since we don't
	# really need it at this point, reap it but leave the net setup around. See:
	# https://github.com/spdk/spdk/issues/2811
	killprocess "$nvmfpid"
}

kernel_target() {
	local name=kernel_target

	configure_kernel_target "$name"
	rabort "$TEST_TRANSPORT" IPv4 "$NVMF_INITIATOR_IP" "$NVMF_PORT" "$name"
	clean_kernel_target
}

nvmftestinit
nvmfappstart -m 0xf

trap 'process_shm --id $NVMF_APP_SHM_ID || :; nvmftestfini || :; clean_kernel_target' SIGINT SIGTERM EXIT

mapfile -t nvmes < <(nvme_in_userspace)
((${#nvmes[@]} > 0))

nvme=${nvmes[0]}

run_test "spdk_target_abort" spdk_target
run_test "kernel_target_abort" kernel_target

trap - SIGINT SIGTERM EXIT
nvmftestfini
