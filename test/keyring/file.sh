#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2024 Intel Corporation.  All rights reserved.

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../..")

set -- "--transport=tcp" "$@"

source "$rootdir/test/common/autotest_common.sh"
source "$testdir/common.sh"

subnqn="nqn.2016-06.io.spdk:cnode0"
hostnqn="nqn.2016-06.io.spdk:host0"
key0="00112233445566778899aabbccddeeff"
key1="112233445566778899aabbccddeeff00"

cleanup() {
	rm -f "$key0path" "$key1path"
	killprocess $bperfpid || :
	killprocess $tgtpid || :
}

trap cleanup EXIT

key0path=$(prep_key "key0" "$key0" 0)
key1path=$(prep_key "key1" "$key1" 0)

"$rootdir/build/bin/spdk_tgt" &
tgtpid=$!

waitforlisten $tgtpid
rpc_cmd << CMD
	nvmf_create_transport -t tcp
	nvmf_create_subsystem "$subnqn"
	bdev_null_create null0 100 4096
	nvmf_subsystem_add_ns "$subnqn" null0
	nvmf_subsystem_add_listener -t tcp -a 127.0.0.1 -s 4420 --secure-channel "$subnqn"
	nvmf_subsystem_add_host --psk "$key0path" "$subnqn" "$hostnqn"
CMD

"$rootdir/build/examples/bdevperf" -q 128 -o 4k -w randrw -M 50 -t 1 -m 2 -r "$bperfsock" -z &
bperfpid=$!

waitforlisten $bperfpid "$bperfsock"
bperf_cmd keyring_file_add_key key0 "$key0path"
bperf_cmd keyring_file_add_key key1 "$key1path"
[[ $(get_key key0 | jq -r '.path') == "$key0path" ]]
[[ $(get_key key1 | jq -r '.path') == "$key1path" ]]
(($(get_refcnt key0) == 1))
(($(get_refcnt key1) == 1))

# Run tests using correct key
bperf_cmd bdev_nvme_attach_controller -b nvme0 -t tcp -a 127.0.0.1 -s 4420 -f ipv4 \
	-n $subnqn -q $hostnqn --psk key0
(($(get_refcnt key0) == 2))
(($(get_refcnt key1) == 1))

"$rootdir/examples/bdev/bdevperf/bdevperf.py" -s "$bperfsock" perform_tests

bperf_cmd bdev_nvme_detach_controller nvme0
(($(get_refcnt key0) == 1))
(($(get_refcnt key1) == 1))

# Try to use wrong key
NOT bperf_cmd bdev_nvme_attach_controller -b nvme0 -t tcp -a 127.0.0.1 -s 4420 -f ipv4 \
	-n $subnqn -q $hostnqn --psk key1
(($(get_refcnt key0) == 1))
(($(get_refcnt key1) == 1))

# Remove both keys
bperf_cmd keyring_file_remove_key key0
bperf_cmd keyring_file_remove_key key1
(($(bperf_cmd keyring_get_keys | jq "length") == 0))

# Try to add a key that has too relaxed permissions
chmod 0660 "$key0path"
NOT bperf_cmd keyring_file_add_key key0 "$key0path"

# Remove the key file after adding it to the keyring
chmod 0600 "$key0path"
bperf_cmd keyring_file_add_key key0 "$key0path"
rm -f "$key0path"
# The key should still be visible
(($(get_refcnt key0) == 1))
# But it shouldn't be possible to use it
NOT bperf_cmd bdev_nvme_attach_controller -b nvme0 -t tcp -a 127.0.0.1 -s 4420 -f ipv4 \
	-n $subnqn -q $hostnqn --psk key0
bperf_cmd keyring_file_remove_key key0

# Remove the key while its in use
key0path=$(prep_key "key0" "$key0" 0)
bperf_cmd keyring_file_add_key key0 "$key0path"
bperf_cmd bdev_nvme_attach_controller -b nvme0 -t tcp -a 127.0.0.1 -s 4420 -f ipv4 \
	-n $subnqn -q $hostnqn --psk key0
(($(get_refcnt key0) == 2))
bperf_cmd keyring_file_remove_key key0
[[ $(get_key key0 | jq -r '.removed') == "true" ]]
(($(get_refcnt key0) == 1))
bperf_cmd bdev_nvme_detach_controller nvme0
(($(bperf_cmd keyring_get_keys | jq "length") == 0))

# Check save/load config
bperf_cmd keyring_file_add_key key0 "$key0path"
bperf_cmd keyring_file_add_key key1 "$key1path"
bperf_cmd bdev_nvme_attach_controller -b nvme0 -t tcp -a 127.0.0.1 -s 4420 -f ipv4 \
	-n $subnqn -q $hostnqn --psk key0

config=$(bperf_cmd save_config)

killprocess $bperfpid
"$rootdir/build/examples/bdevperf" -q 128 -o 4k -w randrw -M 50 -t 1 -m 2 -r "$bperfsock" \
	-z -c <(echo "$config") &
bperfpid=$!

waitforlisten $bperfpid "$bperfsock"
(($(bperf_cmd keyring_get_keys | jq "length") == 2))
(($(get_refcnt key0) == 2))
(($(get_refcnt key1) == 1))
[[ $(bperf_cmd bdev_nvme_get_controllers | jq -r '.[].name') == nvme0 ]]
