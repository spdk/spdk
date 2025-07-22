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
dhgroups=("null" "ffdhe2048" "ffdhe3072" "ffdhe4096" "ffdhe6144" "ffdhe8192")
subnqn="nqn.2024-03.io.spdk:cnode0"
hostnqn="$NVME_HOSTNQN"
hostsock="/var/tmp/host.sock"
keys=() ckeys=()

cleanup() {
	killprocess $hostpid || :
	nvmftestfini || :
	rm -f "${keys[@]}" "${ckeys[@]}" "$output_dir"/nvm{e,f}-auth.log
}

dumplogs() {
	cat "$output_dir/nvme-auth.log"
	cat "$output_dir/nvmf-auth.log"
}

hostrpc() { "$rootdir/scripts/rpc.py" -s "$hostsock" "$@"; }

nvme_connect() {
	# Force 1 I/O queue to speed up the connection and keep ctrlr loss timeout at 0 to ensure
	# immediate disconnect after unsuccessful reauthentication
	nvme connect -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" -n "$subnqn" -i 1 \
		-q "$hostnqn" --hostid "$NVME_HOSTID" -l 0 "$@"
}

nvme_get_ctrlr() {
	local dev

	for dev in /sys/devices/virtual/nvme-fabrics/ctl/nvme*; do
		[[ "$subnqn" == "$(< "$dev/subsysnqn")" ]] && echo "${dev##*/}" && break
	done || false
}

nvme_set_keys() {
	local ctl key ckey dev timeout

	ctl="$1" key="$2" ckey="$3" timeout="$4"
	dev="/sys/devices/virtual/nvme-fabrics/ctl/$ctl"

	[[ -z "$key" ]] || echo "$key" > "$dev/dhchap_secret"
	[[ -z "$ckey" ]] || echo "$ckey" > "$dev/dhchap_ctrl_secret"
	[[ -z "$timeout" ]] || sleep "$timeout"
}

bdev_connect() {
	hostrpc bdev_nvme_attach_controller -t "$TEST_TRANSPORT" -f ipv4 \
		-a "$NVMF_FIRST_TARGET_IP" -s "$NVMF_PORT" -q "$hostnqn" -n "$subnqn" "$@"
}

connect_authenticate() {
	local digest dhgroup key ckey qpairs

	digest="$1" dhgroup="$2" key="key$3"
	ckey=(${ckeys[$3]:+--dhchap-ctrlr-key "ckey$3"})

	rpc_cmd nvmf_subsystem_add_host "$subnqn" "$hostnqn" --dhchap-key "$key" "${ckey[@]}"
	bdev_connect -b "nvme0" --dhchap-key "$key" "${ckey[@]}"

	[[ $(hostrpc bdev_nvme_get_controllers | jq -r '.[].name') == "nvme0" ]]
	qpairs=$(rpc_cmd nvmf_subsystem_get_qpairs "$subnqn")
	[[ $(jq -r ".[0].auth.digest" <<< "$qpairs") == "$digest" ]]
	[[ $(jq -r ".[0].auth.dhgroup" <<< "$qpairs") == "$dhgroup" ]]
	[[ $(jq -r ".[0].auth.state" <<< "$qpairs") == "completed" ]]
	hostrpc bdev_nvme_detach_controller nvme0

	nvme_connect --dhchap-secret "$(< "${keys[$3]}")" \
		${ckeys[$3]:+--dhchap-ctrl-secret "$(< "${ckeys[$3]}")"}
	nvme disconnect -n "$subnqn"
	rpc_cmd nvmf_subsystem_remove_host "$subnqn" "$hostnqn"
}

nvmftestinit
nvmfappstart -L nvmf_auth &> "$output_dir/nvmf-auth.log"
"$rootdir/build/bin/spdk_tgt" -m 2 -r "$hostsock" -L nvme_auth "${NO_HUGE[@]}" &> "$output_dir/nvme-auth.log" &
hostpid=$!

trap "dumplogs; cleanup" SIGINT SIGTERM EXIT

# Set host/ctrlr key pairs with one combination w/o bidirectional authentication
keys[0]=$(gen_dhchap_key "null" 48) ckeys[0]=$(gen_dhchap_key "sha512" 64)
keys[1]=$(gen_dhchap_key "sha256" 32) ckeys[1]=$(gen_dhchap_key "sha384" 48)
keys[2]=$(gen_dhchap_key "sha384" 48) ckeys[2]=$(gen_dhchap_key "sha256" 32)
keys[3]=$(gen_dhchap_key "sha512" 64) ckeys[3]=""

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
	if [[ -n "${ckeys[i]}" ]]; then
		rpc_cmd keyring_file_add_key "ckey$i" "${ckeys[i]}"
		hostrpc keyring_file_add_key "ckey$i" "${ckeys[i]}"
	fi
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
NOT bdev_connect -b "nvme0" --dhchap-key "key2"
rpc_cmd nvmf_subsystem_remove_host "$subnqn" "$hostnqn"

# Check that mismatched controller keys result in failed attach
rpc_cmd nvmf_subsystem_add_host "$subnqn" "$hostnqn" --dhchap-key "key1" --dhchap-ctrlr-key "ckey1"
NOT bdev_connect -b "nvme0" --dhchap-key "key1" --dhchap-ctrlr-key "ckey2"
rpc_cmd nvmf_subsystem_remove_host "$subnqn" "$hostnqn"

# Check that a missing controller key results in a failed attach
rpc_cmd nvmf_subsystem_add_host "$subnqn" "$hostnqn" --dhchap-key "key1"
NOT bdev_connect -b "nvme0" --dhchap-key "key1" --dhchap-ctrlr-key "ckey1"
rpc_cmd nvmf_subsystem_remove_host "$subnqn" "$hostnqn"

# Limit allowed digests/dhgroups on the target
killprocess "$nvmfpid"
nvmfappstart --wait-for-rpc -L nvmf_auth &>> "$output_dir/nvmf-auth.log"
trap "dumplogs; cleanup" SIGINT SIGTERM EXIT

waitforlisten "$nvmfpid"
rpc_cmd <<- CONFIG
	nvmf_set_config --dhchap-digests sha384,sha512 --dhchap-dhgroups ffdhe6144,ffdhe8192
	framework_start_init
	bdev_null_create null0 100 4096
	nvmf_create_transport -t "$TEST_TRANSPORT"
	nvmf_create_subsystem "$subnqn"
	nvmf_subsystem_add_listener -t "$TEST_TRANSPORT" -a "$NVMF_FIRST_TARGET_IP" \
		-s "$NVMF_PORT" "$subnqn"
	nvmf_subsystem_add_ns "$subnqn" "null0" -n 1
CONFIG
for i in "${!keys[@]}"; do
	rpc_cmd keyring_file_add_key "key$i" "${keys[i]}"
	[[ -n "${ckeys[i]}" ]] && rpc_cmd keyring_file_add_key "ckey$i" "${ckeys[i]}"
done

connect_authenticate "sha512" "ffdhe8192" 3

# Check that authentication fails when no common digests are allowed
rpc_cmd nvmf_subsystem_add_host "$subnqn" "$hostnqn" --dhchap-key key3
hostrpc bdev_nvme_set_options --dhchap-digests "sha256"
NOT bdev_connect -b "nvme0" --dhchap-key "key3"

# Check that authentication fails when no common dhgroups are allowed
hostrpc bdev_nvme_set_options --dhchap-dhgroups "ffdhe2048" \
	--dhchap-digests \
	"$(
		IFS=,
		printf "%s" "${digests[*]}"
	)"
NOT bdev_connect -b "nvme0" --dhchap-key "key3"

# Check that the authentication fails when the host wants to authenticate the target (i.e. user set
# the dhchap_ctrlr_key), but the target doesn't require authentication
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
rpc_cmd nvmf_subsystem_remove_host "$subnqn" "$hostnqn"
rpc_cmd nvmf_subsystem_add_host "$subnqn" "$hostnqn"
NOT bdev_connect -b "nvme0" --dhchap-key "key0" --dhchap-ctrlr-key "key1"

# But it's fine when the host key is set and the controller key is not
bdev_connect -b "nvme0" --dhchap-key "key0"
[[ $(hostrpc bdev_nvme_get_controllers | jq -r '.[].name') == "nvme0" ]]
hostrpc bdev_nvme_detach_controller nvme0

# Check changing the keys
rpc_cmd nvmf_subsystem_set_keys "$subnqn" "$hostnqn" --dhchap-key "key1"
bdev_connect -b "nvme0" --dhchap-key "key1"
[[ $(hostrpc bdev_nvme_get_controllers | jq -r '.[].name') == "nvme0" ]]
# Change the keys again - this shouldn't affect existing connections
rpc_cmd nvmf_subsystem_set_keys "$subnqn" "$hostnqn" --dhchap-key "key2" --dhchap-ctrlr-key "key3"
[[ $(hostrpc bdev_nvme_get_controllers | jq -r '.[].name') == "nvme0" ]]
# But new connections require new keys
nvme_connect --dhchap-secret "$(< ${keys[2]})" --dhchap-ctrl-secret "$(< ${keys[3]})"
nctrlr=$(nvme_get_ctrlr)
hostrpc bdev_nvme_detach_controller nvme0
NOT bdev_connect -b "nvme0" --dhchap-key "key1"
bdev_connect -b "nvme0" --dhchap-key "key2" --dhchap-ctrlr-key "key3"
[[ $(hostrpc bdev_nvme_get_controllers | jq -r '.[].name') == "nvme0" ]]
hostrpc bdev_nvme_detach_controller nvme0
# Clear the keys and check that the host is no longer required to authenticate
rpc_cmd nvmf_subsystem_set_keys "$subnqn" "$hostnqn"
bdev_connect -b "nvme0"
[[ $(hostrpc bdev_nvme_get_controllers | jq -r '.[].name') == "nvme0" ]]
hostrpc bdev_nvme_detach_controller nvme0
# Change the keys on the target once again, but this time force reauthentication on the already
# established connections
rpc_cmd nvmf_subsystem_set_keys "$subnqn" "$hostnqn" --dhchap-key "key1" --dhchap-ctrlr-key "key3"
nvme_set_keys "$nctrlr" "$(< ${keys[1]})" "" 2s
waitforblk "${nctrlr}n1"
# Change the ctrlr key
rpc_cmd nvmf_subsystem_set_keys "$subnqn" "$hostnqn" --dhchap-key "key1" --dhchap-ctrlr-key "key2"
nvme_set_keys "$nctrlr" "" "$(< ${keys[2]})" 2s
waitforblk "${nctrlr}n1"
nvme disconnect -n "$subnqn"

# Reauthenticate using bdev_nvme
rpc_cmd nvmf_subsystem_set_keys "$subnqn" "$hostnqn" --dhchap-key "key0" --dhchap-ctrlr-key "key1"
bdev_connect -b "nvme0" --dhchap-key "key0" --dhchap-ctrlr-key "key1" --ctrlr-loss-timeout-sec 1 \
	--reconnect-delay-sec 1
rpc_cmd nvmf_subsystem_set_keys "$subnqn" "$hostnqn" --dhchap-key "key2" --dhchap-ctrlr-key "key3"
hostrpc bdev_nvme_set_keys "nvme0" --dhchap-key "key2" --dhchap-ctrlr-key "key3"
[[ $(hostrpc bdev_nvme_get_controllers | jq -r '.[].name') == "nvme0" ]]
# Clear the keys while connected
rpc_cmd nvmf_subsystem_set_keys "$subnqn" "$hostnqn"
hostrpc bdev_nvme_set_keys "nvme0"
[[ $(hostrpc bdev_nvme_get_controllers | jq -r '.[].name') == "nvme0" ]]
# Use wrong keys and verify that the ctrlr will get disconnected after ctrlr-loss-timeout-sec
rpc_cmd nvmf_subsystem_set_keys "$subnqn" "$hostnqn" --dhchap-key "key2" --dhchap-ctrlr-key "key3"
NOT hostrpc bdev_nvme_set_keys "nvme0" --dhchap-key "key1" --dhchap-ctrlr-key "key3"
while (($(hostrpc bdev_nvme_get_controllers | jq 'length') != 0)); do
	sleep 1s
done

# Do the same, but this time try with a valid host key, but bad ctrlr key
rpc_cmd nvmf_subsystem_set_keys "$subnqn" "$hostnqn" --dhchap-key "key0" --dhchap-ctrlr-key "key1"
bdev_connect -b "nvme0" --dhchap-key "key0" --dhchap-ctrlr-key "key1" --ctrlr-loss-timeout-sec 1 \
	--reconnect-delay-sec 1
rpc_cmd nvmf_subsystem_set_keys "$subnqn" "$hostnqn" --dhchap-key "key2" --dhchap-ctrlr-key "key3"
NOT hostrpc bdev_nvme_set_keys "nvme0" --dhchap-key "key2" --dhchap-ctrlr-key "key0"
while (($(hostrpc bdev_nvme_get_controllers | jq 'length') != 0)); do
	sleep 1s
done

trap - SIGINT SIGTERM EXIT
cleanup
