#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2023 Intel Corporation
# All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

# Use TCP as the default nvmf transport
TEST_TRANSPORT=${TEST_TRANSPORT:=tcp}

source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

nqn=nqn.2016-06.io.spdk:cnode0
key0=(00112233445566778899001122334455 11223344556677889900112233445500)
key1=(22334455667788990011223344550011 33445566778899001122334455001122)
bperfsock=/var/tmp/bperf.sock
declare -A stats

rpc_bperf() { "$rootdir/scripts/rpc.py" -s "$bperfsock" "$@"; }

spdk_dd() {
	local config

	# Disable auto-examine to avoid seeing the examine callbacks' reads in accel stats
	config=$("$rootdir/scripts/gen_nvme.sh" --mode=remote --json-with-subsystems \
		--trid="$TEST_TRANSPORT:$NVMF_FIRST_TARGET_IP:$NVMF_PORT:$nqn" \
		| jq '.subsystems[0].config[.subsystems[0].config | length] |=
			{"method": "bdev_set_options", "params": {"bdev_auto_examine": false}}')

	"$rootdir/build/bin/spdk_dd" -c <(echo "$config") "$@"
}

get_stat() {
	local event opcode rpc

	event="$1" opcode="$2" rpc=${3:-rpc_cmd}
	if [[ -z "$opcode" ]]; then
		"$rpc" accel_get_stats | jq -r ".$event"
	else
		"$rpc" accel_get_stats \
			| jq -r ".operations[] | select(.opcode == \"$opcode\").$event"
	fi
}

get_stat_bperf() { get_stat "$1" "$2" rpc_bperf; }

update_stats() {
	stats["sequence_executed"]=$(get_stat sequence_executed)
	stats["encrypt_executed"]=$(get_stat executed encrypt)
	stats["decrypt_executed"]=$(get_stat executed decrypt)
	stats["copy_executed"]=$(get_stat executed copy)
}

tgtcleanup() {
	rm -f "$input" "$output"
	nvmftestfini
}

bperfcleanup() {
	[[ -n "$bperfpid" ]] && killprocess $bperfpid
}

nvmftestinit
nvmfappstart -m 0x2

input=$(mktemp) output=$(mktemp)
trap 'tgtcleanup; exit 1' SIGINT SIGTERM EXIT

rpc_cmd <<- CONFIG
	bdev_malloc_create 32 4096 -b malloc0
	accel_crypto_key_create -c AES_XTS -k "${key0[0]}" -e "${key0[1]}" -n key0
	accel_crypto_key_create -c AES_XTS -k "${key1[0]}" -e "${key1[1]}" -n key1
	bdev_crypto_create malloc0 crypto0 -n key0
	bdev_crypto_create crypto0 crypto1 -n key1
	nvmf_create_transport $NVMF_TRANSPORT_OPTS
	nvmf_create_subsystem $nqn -a
	nvmf_subsystem_add_listener $nqn -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
	nvmf_subsystem_add_ns $nqn crypto1
CONFIG

# Remember initial stats
update_stats

# Write a single 64K request and check the stats
dd if=/dev/urandom of="$input" bs=1K count=64
spdk_dd --if "$input" --ob Nvme0n1 --bs $((64 * 1024)) --count 1
(($(get_stat sequence_executed) == stats["sequence_executed"] + 1))
(($(get_stat executed encrypt) == stats["encrypt_executed"] + 2))
(($(get_stat executed decrypt) == stats["decrypt_executed"]))
# No copies should be done - the copy from the malloc should translate to changing encrypt's
# destination buffer
(($(get_stat executed copy) == stats["copy_executed"]))
update_stats

# Now read that 64K, verify the stats and check that it matches what was written
spdk_dd --of "$output" --ib Nvme0n1 --bs $((64 * 1024)) --count 1
(($(get_stat sequence_executed) == stats["sequence_executed"] + 1))
(($(get_stat executed encrypt) == stats["encrypt_executed"]))
(($(get_stat executed decrypt) == stats["decrypt_executed"] + 2))
(($(get_stat executed copy) == stats["copy_executed"]))
cmp "$input" "$output"
spdk_dd --if /dev/zero --ob Nvme0n1 --bs $((64 * 1024)) --count 1
update_stats

# Now do the same using 4K requests
spdk_dd --if "$input" --ob Nvme0n1 --bs 4096 --count 16
(($(get_stat sequence_executed) == stats["sequence_executed"] + 16))
(($(get_stat executed encrypt) == stats["encrypt_executed"] + 32))
(($(get_stat executed decrypt) == stats["decrypt_executed"]))
(($(get_stat executed copy) == stats["copy_executed"]))
update_stats

# Check the reads
: > "$output"
spdk_dd --of "$output" --ib Nvme0n1 --bs 4096 --count 16
(($(get_stat sequence_executed) == stats["sequence_executed"] + 16))
(($(get_stat executed encrypt) == stats["encrypt_executed"]))
(($(get_stat executed decrypt) == stats["decrypt_executed"] + 32))
(($(get_stat executed copy) == stats["copy_executed"]))
cmp "$input" "$output"

trap - SIGINT SIGTERM EXIT
tgtcleanup

# Verify bdev crypto ENOMEM handling by setting low accel task count and sending IO with high qd
trap 'bperfcleanup; exit 1' SIGINT SIGTERM EXIT

"$rootdir/build/examples/bdevperf" -t 5 -w verify -o 4096 -q 256 --wait-for-rpc -z &
bperfpid=$!

waitforlisten $bperfpid
rpc_cmd <<- CONFIG
	accel_set_options --task-count 16
	framework_start_init
	bdev_malloc_create 32 4096 -b malloc0
	accel_crypto_key_create -c AES_XTS -k "${key0[0]}" -e "${key0[1]}" -n key0
	accel_crypto_key_create -c AES_XTS -k "${key1[0]}" -e "${key1[1]}" -n key1
	bdev_crypto_create malloc0 crypto0 -n key0
	bdev_crypto_create crypto0 crypto1 -n key1
CONFIG

"$rootdir/examples/bdev/bdevperf/bdevperf.py" perform_tests
killprocess $bperfpid

# Verify ENOMEM handling in the bdev layer by using the same accel configuration, but adding a
# passthru bdev, which doesn't support memory domains/accel, to force bdev layer to append copies
# and execute accel sequences
"$rootdir/build/examples/bdevperf" -t 5 -w verify -o 4096 -q 256 --wait-for-rpc -z &
bperfpid=$!

waitforlisten $bperfpid
rpc_cmd <<- CONFIG
	accel_set_options --task-count 16
	framework_start_init
	bdev_malloc_create 32 4096 -b malloc0
	accel_crypto_key_create -c AES_XTS -k "${key0[0]}" -e "${key0[1]}" -n key0
	accel_crypto_key_create -c AES_XTS -k "${key1[0]}" -e "${key1[1]}" -n key1
	bdev_passthru_create -p pt0 -b malloc0
	bdev_crypto_create pt0 crypto0 -n key0
	bdev_crypto_create crypto0 crypto1 -n key1
CONFIG

"$rootdir/examples/bdev/bdevperf/bdevperf.py" perform_tests
killprocess $bperfpid

trap - SIGINT SIGTERM EXIT
killprocess $bperfpid
wait $bperfpid

# Check integration with the TCP transport in the NVMe driver by running I/O with data
# digest enabled
nvmftestinit
nvmfappstart -m 0x2

rpc_cmd <<- CONFIG
	bdev_malloc_create 32 4096 -b malloc0
	nvmf_create_transport -t tcp
	nvmf_create_subsystem $nqn -a
	nvmf_subsystem_add_listener $nqn -t tcp -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
	nvmf_subsystem_add_ns $nqn malloc0
CONFIG

trap 'bperfcleanup || :; nvmftestfini || :; exit 1' SIGINT SIGTERM EXIT
"$rootdir/build/examples/bdevperf" -r "$bperfsock" -t 5 -w verify -o 4096 -q 256 \
	--wait-for-rpc -z &
bperfpid=$!

waitforlisten $bperfpid "$bperfsock"
rpc_bperf <<- CONFIG
	bdev_set_options --disable-auto-examine
	bdev_nvme_set_options --allow-accel-sequence
	framework_start_init
	bdev_nvme_attach_controller -t tcp -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -f ipv4 -n $nqn -b nvme0 --ddgst
	accel_crypto_key_create -c AES_XTS -k "${key0[0]}" -e "${key0[1]}" -n key0
	bdev_crypto_create nvme0n1 crypto0 -n key0
CONFIG

"$rootdir/examples/bdev/bdevperf/bdevperf.py" -s "$bperfsock" perform_tests

# Check the stats and verify that sequence is executed once for all operations (either encrypt+crc32
# or decrypt+crc32)
sequence=$(get_stat_bperf sequence_executed)
encrypt=$(get_stat_bperf executed encrypt)
decrypt=$(get_stat_bperf executed decrypt)
crc32c=$(get_stat_bperf executed crc32c)

((sequence > 0))
((encrypt + decrypt == sequence))
((encrypt + decrypt == crc32c))

killprocess $bperfpid

# Check the same with a larger block size to verify the digest calculation without in-capsule data
"$rootdir/build/examples/bdevperf" -r "$bperfsock" -t 5 -w verify -o $((64 * 1024)) -q 32 \
	--wait-for-rpc -z &
bperfpid=$!

waitforlisten $bperfpid "$bperfsock"
rpc_bperf <<- CONFIG
	bdev_set_options --disable-auto-examine
	bdev_nvme_set_options --allow-accel-sequence
	framework_start_init
	bdev_nvme_attach_controller -t tcp -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -f ipv4 -n $nqn -b nvme0 --ddgst
	accel_crypto_key_create -c AES_XTS -k "${key0[0]}" -e "${key0[1]}" -n key0
	bdev_crypto_create nvme0n1 crypto0 -n key0
CONFIG

"$rootdir/examples/bdev/bdevperf/bdevperf.py" -s "$bperfsock" perform_tests

sequence=$(get_stat_bperf sequence_executed)
encrypt=$(get_stat_bperf executed encrypt)
decrypt=$(get_stat_bperf executed decrypt)
crc32c=$(get_stat_bperf executed crc32c)

((sequence > 0))
((encrypt + decrypt == sequence))
((encrypt + decrypt == crc32c))

killprocess $bperfpid
nvmftestfini

trap - SIGINT SIGTERM EXIT
