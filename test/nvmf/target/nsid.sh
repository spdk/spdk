#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2024 Intel Corporation

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../../")

source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

subnqn1="nqn.2024-10.io.spdk:cnode0"
subnqn2="nqn.2024-10.io.spdk:cnode1"
subnqn3="nqn.2024-10.io.spdk:cnode2"
tgt2sock="/var/tmp/tgt2.sock"
tgt2pid=

cleanup() {
	killprocess $tgt2pid || :
	nvmftestfini || :
}

nvme_connect() {
	local ctrlr

	nvme connect -t "$TEST_TRANSPORT" -a "$tgt2addr" -s "$NVMF_SECOND_PORT" -n "$subnqn3" \
		"${NVME_HOST[@]}" "$@" >&2

	for ctrlr in /sys/class/nvme/nvme*; do
		# shellcheck disable=SC2053
		[[ -e $ctrlr/subsysnqn && $(< "$ctrlr/subsysnqn") == "$subnqn3" ]] || continue
		echo "${ctrlr##*/}"
		return 0
	done

	nvme disconnect -n "$subnqn3" >&2
	return 1
}

nvme_get_nguid() {
	local ctrlr=$1 nsid=$2 nguid ctrlr_id
	ctrlr_id=$(echo $ctrlr | grep -o '[0-9]')

	for ns in "/sys/class/nvme/${ctrlr}/${ctrlr}c${ctrlr_id}n"*; do
		[[ -e $ns/nsid && $(< "$ns/nsid") == "$nsid" ]] || continue
		nguid=$(cat $ns/nguid)
		uuid2nguid ${nguid^^}
		return 0
	done

	return 1
}

nvmftestinit
nvmfappstart -m 1

trap cleanup SIGINT SIGTERM EXIT

"$rootdir/build/bin/spdk_tgt" -m 2 -r "$tgt2sock" "${NO_HUGE[@]}" &
tgt2pid=$!

tgt1addr="$NVMF_FIRST_TARGET_IP"
tgt2addr="$(get_main_ns_ip)"
ns1uuid=$(uuidgen)
ns2uuid=$(uuidgen)
ns3uuid=$(uuidgen)

# Start up two targets, both exposing 3 namespaces. Target #2 connects to the subsystems from
# target #1 and configures its namespaces as follows: nvme0n1 -> nsid:2, nvme0n2 -> nsid:1,
# nvme1n1 -> nsid:3.
rpc_cmd <<- CONFIG
	bdev_null_create null0 100 4096 -u "$ns2uuid"
	bdev_null_create null1 100 4096 -u "$ns1uuid"
	bdev_null_create null2 100 4096 -u "$ns3uuid"
	nvmf_create_transport -t "$TEST_TRANSPORT"
	nvmf_create_subsystem "$subnqn1" -a
	nvmf_create_subsystem "$subnqn2" -a
	nvmf_subsystem_add_listener -t "$TEST_TRANSPORT" -a "$tgt1addr" \
		-s "$NVMF_PORT" "$subnqn1"
	nvmf_subsystem_add_listener -t "$TEST_TRANSPORT" -a "$tgt1addr" \
		-s "$NVMF_PORT" "$subnqn2"
	nvmf_subsystem_add_ns "$subnqn1" null0 -n 1
	nvmf_subsystem_add_ns "$subnqn1" null1 -n 2
	nvmf_subsystem_add_ns "$subnqn2" null2 -n 1
CONFIG

waitforlisten "$tgt2pid" "$tgt2sock"
"$rootdir/scripts/rpc.py" -s "$tgt2sock" <<- CONFIG
	bdev_nvme_attach_controller -t "$TEST_TRANSPORT" -a "$tgt1addr" \
		-s "$NVMF_PORT" -f ipv4 -n "$subnqn1" -b nvme0
	bdev_nvme_attach_controller -t "$TEST_TRANSPORT" -a "$tgt1addr" \
		-s "$NVMF_PORT" -f ipv4 -n "$subnqn2" -b nvme1
	nvmf_create_transport -t "$TEST_TRANSPORT"
	nvmf_create_subsystem "$subnqn3" -a
	nvmf_subsystem_add_listener -t "$TEST_TRANSPORT" -a "$tgt2addr" \
		-s "$NVMF_SECOND_PORT" "$subnqn3"
	nvmf_subsystem_add_ns "$subnqn3" nvme0n2 -n 1
	nvmf_subsystem_add_ns "$subnqn3" nvme0n1 -n 2
	nvmf_subsystem_add_ns "$subnqn3" nvme1n1 -n 3
CONFIG
# Ensure that the nguids returned in id-ns match target #1's configuration
ctrlr="$(nvme_connect)"
waitforblk "${ctrlr}n1"
[[ "$(uuid2nguid "$ns1uuid")" == "$(nvme_get_nguid "$ctrlr" 1)" ]]
waitforblk "${ctrlr}n2"
[[ "$(uuid2nguid "$ns2uuid")" == "$(nvme_get_nguid "$ctrlr" 2)" ]]
waitforblk "${ctrlr}n3"
[[ "$(uuid2nguid "$ns3uuid")" == "$(nvme_get_nguid "$ctrlr" 3)" ]]
nvme disconnect -d "/dev/$ctrlr"

trap - SIGINT SIGTERM EXIT
cleanup
