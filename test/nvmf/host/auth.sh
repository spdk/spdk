#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2023 Intel Corporation.  All rights reserved.
#

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../../")

source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

# shellcheck disable=SC2190
digests=("sha256" "sha384" "sha512")
# There's a bug in the kernel with the way dhgroups are negotiated that makes it impossible to
# select null dhgroup, so skip it for now.
dhgroups=("ffdhe2048" "ffdhe3072" "ffdhe4096" "ffdhe6144" "ffdhe8192")
subnqn="nqn.2024-02.io.spdk:cnode0"
hostnqn="nqn.2024-02.io.spdk:host0"
nvmet_subsys="/sys/kernel/config/nvmet/subsystems/$subnqn"
nvmet_host="/sys/kernel/config/nvmet/hosts/$hostnqn"
keys=() ckeys=()

cleanup() {
	nvmftestfini || :
	rm "$nvmet_subsys/allowed_hosts/$hostnqn" || :
	rmdir "$nvmet_host" || :
	clean_kernel_target || :
	rm -f "${keys[@]}" "$output_dir/nvme-auth.log"
	# configure_kernel_target() binds the SSDs to the kernel driver, so move them back to
	# userspace, as this is what the tests running after this one expect
	"$rootdir/scripts/setup.sh"
}

nvmet_auth_init() {
	configure_kernel_target "$subnqn" "$(get_main_ns_ip)"
	mkdir "$nvmet_host"
	echo 0 > "$nvmet_subsys/attr_allow_any_host"
	ln -s "$nvmet_host" "$nvmet_subsys/allowed_hosts/$hostnqn"
}

nvmet_auth_set_key() {
	local digest dhgroup keyid key ckey

	digest="$1" dhgroup="$2" keyid="$3"
	key=$(< "${keys[keyid]}")
	ckey=${ckeys[keyid]:+$(< ${ckeys[keyid]})}

	echo "hmac($digest)" > "$nvmet_host/dhchap_hash"
	echo "$dhgroup" > "$nvmet_host/dhchap_dhgroup"
	echo "$key" > "$nvmet_host/dhchap_key"
	[[ -z "$ckey" ]] || echo "$ckey" > "$nvmet_host/dhchap_ctrl_key"
}

connect_authenticate() {
	local digest dhgroup keyid ckey

	digest="$1" dhgroup="$2" keyid="$3"
	ckey=(${ckeys[keyid]:+--dhchap-ctrlr-key "ckey${keyid}"})

	rpc_cmd bdev_nvme_set_options --dhchap-digests "$digest" --dhchap-dhgroups "$dhgroup"
	rpc_cmd bdev_nvme_attach_controller -b nvme0 -t "$TEST_TRANSPORT" -f ipv4 \
		-a "$(get_main_ns_ip)" -s "$NVMF_PORT" -q "$hostnqn" -n "$subnqn" \
		--dhchap-key "key${keyid}" "${ckey[@]}"
	[[ $(rpc_cmd bdev_nvme_get_controllers | jq -r '.[].name') == "nvme0" ]]
	rpc_cmd bdev_nvme_detach_controller nvme0
}

nvmftestinit
nvmfappstart -L nvme_auth &> "$output_dir/nvme-auth.log"
trap "cat "$output_dir/nvme-auth.log"; cleanup" SIGINT SIGTERM EXIT

# Set host/ctrlr key pairs with one combination w/o bidirectional authentication
keys[0]=$(gen_dhchap_key "null" 32) ckeys[0]=$(gen_dhchap_key "sha512" 64)
keys[1]=$(gen_dhchap_key "null" 48) ckeys[1]=$(gen_dhchap_key "sha384" 48)
keys[2]=$(gen_dhchap_key "sha256" 32) ckeys[2]=$(gen_dhchap_key "sha256" 32)
keys[3]=$(gen_dhchap_key "sha384" 48) ckeys[3]=$(gen_dhchap_key "null" 32)
keys[4]=$(gen_dhchap_key "sha512" 64) ckeys[4]=""

waitforlisten "$nvmfpid"
for i in "${!keys[@]}"; do
	rpc_cmd keyring_file_add_key "key$i" "${keys[i]}"
	[[ -n "${ckeys[i]}" ]] && rpc_cmd keyring_file_add_key "ckey$i" "${ckeys[i]}"
done

nvmet_auth_init

# Connect with all digests/dhgroups enabled
nvmet_auth_set_key "sha256" "ffdhe2048" 1
connect_authenticate \
	"$(
		IFS=,
		printf "%s" "${digests[*]}"
	)" \
	"$(
		IFS=,
		printf "%s" "${dhgroups[*]}"
	)" 1

# Check all digest/dhgroup/key combinations
for digest in "${digests[@]}"; do
	for dhgroup in "${dhgroups[@]}"; do
		for keyid in "${!keys[@]}"; do
			nvmet_auth_set_key "$digest" "$dhgroup" "$keyid"
			connect_authenticate "$digest" "$dhgroup" "$keyid"
		done
	done
done

# Ensure that a missing key results in failed attach
nvmet_auth_set_key "sha256" "ffdhe2048" 1
rpc_cmd bdev_nvme_set_options --dhchap-digests "sha256" --dhchap-dhgroups "ffdhe2048"
NOT rpc_cmd bdev_nvme_attach_controller -b nvme0 -t "$TEST_TRANSPORT" -f ipv4 \
	-a "$(get_main_ns_ip)" -s "$NVMF_PORT" -q "$hostnqn" -n "$subnqn"
(($(rpc_cmd bdev_nvme_get_controllers | jq 'length') == 0))

# Check that mismatched keys result in failed attach
NOT rpc_cmd bdev_nvme_attach_controller -b nvme0 -t "$TEST_TRANSPORT" -f ipv4 \
	-a "$(get_main_ns_ip)" -s "$NVMF_PORT" -q "$hostnqn" -n "$subnqn" \
	--dhchap-key "key2"
(($(rpc_cmd bdev_nvme_get_controllers | jq 'length') == 0))

# Check that a failed controller authentication results in failed attach
NOT rpc_cmd bdev_nvme_attach_controller -b nvme0 -t "$TEST_TRANSPORT" -f ipv4 \
	-a "$(get_main_ns_ip)" -s "$NVMF_PORT" -q "$hostnqn" -n "$subnqn" \
	--dhchap-key "key1" --dhchap-ctrlr-key "ckey2"

# Check reauthentication
rpc_cmd bdev_nvme_attach_controller -b nvme0 -t "$TEST_TRANSPORT" -f ipv4 \
	-a "$(get_main_ns_ip)" -s "$NVMF_PORT" -q "$hostnqn" -n "$subnqn" \
	--dhchap-key "key1" --dhchap-ctrlr-key "ckey1" --ctrlr-loss-timeout-sec 1 \
	--reconnect-delay-sec 1
nvmet_auth_set_key "sha256" "ffdhe2048" 2
rpc_cmd bdev_nvme_set_keys "nvme0" --dhchap-key "key2" --dhchap-ctrlr-key "ckey2"
[[ $(rpc_cmd bdev_nvme_get_controllers | jq -r '.[].name') == "nvme0" ]]
# Use wrong keys and verify that the ctrlr will get disconnected after ctrlr-loss-timeout-sec
NOT rpc_cmd bdev_nvme_set_keys "nvme0" --dhchap-key "key1" --dhchap-ctrlr-key "ckey2"
while (($(rpc_cmd bdev_nvme_get_controllers | jq 'length') != 0)); do
	sleep 1s
done
# Do the same, but this time try with a valid host key, but bad ctrlr key
nvmet_auth_set_key "sha256" "ffdhe2048" 1
rpc_cmd bdev_nvme_attach_controller -b nvme0 -t "$TEST_TRANSPORT" -f ipv4 \
	-a "$(get_main_ns_ip)" -s "$NVMF_PORT" -q "$hostnqn" -n "$subnqn" \
	--dhchap-key "key1" --dhchap-ctrlr-key "ckey1" --ctrlr-loss-timeout-sec 1 \
	--reconnect-delay-sec 1
nvmet_auth_set_key "sha256" "ffdhe2048" 2
NOT rpc_cmd bdev_nvme_set_keys "nvme0" --dhchap-key "key2" --dhchap-ctrlr-key "ckey1"
while (($(rpc_cmd bdev_nvme_get_controllers | jq 'length') != 0)); do
	sleep 1s
done

trap - SIGINT SIGTERM EXIT
cleanup
