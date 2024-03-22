#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2024 Intel Corporation
#

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../../")

source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

# shellcheck disable=SC2190
digests=("sha256" "sha384" "sha512")
dhgroups=("null")
subnqn="nqn.2024-03.io.spdk:cnode0"
hostnqn="$NVME_HOSTNQN"
hostsock="/var/tmp/host.sock"
keys=()

cleanup() {
	killprocess $hostpid || :
	nvmftestfini || :
	rm -f "${keys[@]}" "$output_dir"/nvm{e,f}-auth.log
}

dumplogs() {
	cat "$output_dir/nvme-auth.log"
	cat "$output_dir/nvmf-auth.log"
}

hostrpc() { "$rootdir/scripts/rpc.py" -s "$hostsock" "$@"; }

connect_authenticate() {
	local digest dhgroup key qpairs

	digest="$1" dhgroup="$2" key="key$3"

	rpc_cmd nvmf_subsystem_add_host "$subnqn" "$hostnqn" --dhchap-key "$key"
	hostrpc bdev_nvme_attach_controller -b nvme0 -t "$TEST_TRANSPORT" -f ipv4 \
		-a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -q "$hostnqn" -n "$subnqn" \
		--dhchap-key "${key}"

	[[ $(hostrpc bdev_nvme_get_controllers | jq -r '.[].name') == "nvme0" ]]
	qpairs=$(rpc_cmd nvmf_subsystem_get_qpairs "$subnqn")
	[[ $(jq -r ".[0].auth.digest" <<< "$qpairs") == "$digest" ]]
	[[ $(jq -r ".[0].auth.dhgroup" <<< "$qpairs") == "$dhgroup" ]]
	[[ $(jq -r ".[0].auth.state" <<< "$qpairs") == "completed" ]]
	hostrpc bdev_nvme_detach_controller nvme0

	# Force 1 I/O queue to speed up the connection
	nvme connect -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -n "$subnqn" -i 1 \
		-q "$hostnqn" --hostid "$NVME_HOSTID" --dhchap-secret "$(< "${keys[$3]}")"
	nvme disconnect -n "$subnqn"
	rpc_cmd nvmf_subsystem_remove_host "$subnqn" "$hostnqn"
}

nvmftestinit
nvmfappstart -L nvmf_auth &> "$output_dir/nvmf-auth.log"
"$rootdir/build/bin/spdk_tgt" -m 2 -r "$hostsock" -L nvme_auth &> "$output_dir/nvme-auth.log" &
hostpid=$!

trap "dumplogs; cleanup" SIGINT SIGTERM EXIT

keys[0]=$(gen_dhchap_key "null" 48)
keys[1]=$(gen_dhchap_key "sha256" 32)
keys[2]=$(gen_dhchap_key "sha384" 48)
keys[3]=$(gen_dhchap_key "sha512" 64)

waitforlisten "$nvmfpid"
waitforlisten "$hostpid" "$hostsock"
rpc_cmd <<- CONFIG
	nvmf_create_transport -t "$TEST_TRANSPORT"
	nvmf_create_subsystem "$subnqn"
	nvmf_subsystem_add_listener -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" \
		-s "$NVMF_PORT" "$subnqn"
CONFIG

for i in "${!keys[@]}"; do
	rpc_cmd keyring_file_add_key "key$i" "${keys[i]}"
	hostrpc keyring_file_add_key "key$i" "${keys[i]}"
done

# Check all digest/dhgroup/key combinations
for digest in "${digests[@]}"; do
	for dhgroup in "${dhgroups[@]}"; do
		for keyid in "${!keys[@]}"; do
			hostrpc bdev_nvme_set_options --dhchap-digests "$digest" \
				--dhchap-dhgroups "$dhgroup"
			connect_authenticate "$digest" "$dhgroup" $keyid
		done
	done
done

# Connect with all digests/dhgroups enabled
hostrpc bdev_nvme_set_options \
	--dhchap-digests \
	"$(
		IFS=,
		printf "%s" "${digests[*]}"
	)" \
	--dhchap-dhgroups \
	"$(
		IFS=,
		printf "%s" "${dhgroups[*]}"
	)"
# The target should select the strongest digest/dhgroup
connect_authenticate "${digests[-1]}" "${dhgroups[-1]}" 0

# Check that mismatched keys result in failed attach
rpc_cmd nvmf_subsystem_add_host "$subnqn" "$hostnqn" --dhchap-key "key1"
NOT hostrpc bdev_nvme_attach_controller -b nvme0 -t "$TEST_TRANSPORT" -f ipv4 \
	-a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -q "$hostnqn" -n "$subnqn" \
	--dhchap-key "key2"
rpc_cmd nvmf_subsystem_remove_host "$subnqn" "$hostnqn"

trap - SIGINT SIGTERM EXIT
cleanup
