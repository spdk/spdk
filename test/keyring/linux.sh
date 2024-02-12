#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2024 Intel Corporation.  All rights reserved.

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../..")

source "$rootdir/test/common/autotest_common.sh"
source "$testdir/common.sh"

subnqn="nqn.2016-06.io.spdk:cnode0"
hostnqn="nqn.2016-06.io.spdk:host0"
key0="00112233445566778899aabbccddeeff"
key1="112233445566778899aabbccddeeff00"

get_keysn() { keyctl search @s user "$1"; }

check_keys() {
	local count=$1 name=$2
	local sn

	(($(bperf_cmd keyring_get_keys | jq 'length') == count))
	((count == 0)) && return

	sn=$(get_key "$name" | jq -r ".sn")
	[[ $(get_keysn $name) == "$sn" ]]
	[[ $(keyctl print "$sn") == "$(< /tmp/$name)" ]]
}

unlink_key() {
	local name=$1 sn

	sn=$(get_keysn ":spdk-test:$name")
	keyctl unlink "$sn"
}

cleanup() {
	for key in key0 key1; do
		unlink_key $key || :
	done
	killprocess $bperfpid || :
	killprocess $tgtpid || :
}

trap cleanup EXIT

prep_key "key0" "$key0" 0 "/tmp/:spdk-test:key0"
prep_key "key1" "$key1" 0 "/tmp/:spdk-test:key1"

"$rootdir/build/bin/spdk_tgt" &
tgtpid=$!

waitforlisten $tgtpid
rpc_cmd << CMD
	nvmf_create_transport -t tcp
	nvmf_create_subsystem $subnqn
	bdev_null_create null0 100 4096
	nvmf_subsystem_add_ns $subnqn null0
	nvmf_subsystem_add_listener -t tcp -a 127.0.0.1 -s 4420 --secure-channel $subnqn
	keyring_file_add_key key0 /tmp/:spdk-test:key0
	nvmf_subsystem_add_host --psk key0 $subnqn $hostnqn
CMD

# Add a valid key to kernel's keyring and verify that it's possible to use it to establish TLS
# connection
keyctl add user ":spdk-test:key0" "$(< /tmp/:spdk-test:key0)" @s
keyctl add user ":spdk-test:key1" "$(< /tmp/:spdk-test:key1)" @s
"$rootdir/build/examples/bdevperf" -q 128 -o 4k -w randread -t 1 -m 2 \
	-r "$bperfsock" -z --wait-for-rpc &
bperfpid=$!

waitforlisten $bperfpid "$bperfsock"
bperf_cmd keyring_linux_set_options --enable
bperf_cmd framework_start_init
bperf_cmd bdev_nvme_attach_controller -b nvme0 -t tcp -a 127.0.0.1 -s 4420 -f ipv4 \
	-n $subnqn -q $hostnqn --psk ":spdk-test:key0"
check_keys 1 ":spdk-test:key0"

"$rootdir/examples/bdev/bdevperf/bdevperf.py" -s "$bperfsock" perform_tests
bperf_cmd bdev_nvme_detach_controller nvme0
check_keys 0

# Try to use wrong key
NOT bperf_cmd bdev_nvme_attach_controller -b nvme0 -t tcp -a 127.0.0.1 -s 4420 -f ipv4 \
	-n $subnqn -q $hostnqn --psk ":spdk-test:key1"
