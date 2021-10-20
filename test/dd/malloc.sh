#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")
source "$testdir/common.sh"

malloc_copy() {
	local mbdev0=malloc0 mbdev0_b=1048576 mbdev0_bs=512
	local mbdev1=malloc1 mbdev1_b=1048576 mbdev1_bs=512

	local -A method_bdev_malloc_create_0=(
		["name"]=$mbdev0
		["num_blocks"]=$mbdev0_b
		["block_size"]=$mbdev0_bs
	)

	local -A method_bdev_malloc_create_1=(
		["name"]=$mbdev1
		["num_blocks"]=$mbdev1_b
		["block_size"]=$mbdev1_bs
	)

	"${DD_APP[@]}" \
		--ib="$mbdev0" \
		--ob="$mbdev1" \
		--json <(gen_conf)

	"${DD_APP[@]}" \
		--ib="$mbdev1" \
		--ob="$mbdev0" \
		--json <(gen_conf)
}

run_test "dd_malloc_copy" malloc_copy
