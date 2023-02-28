#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../../")
# Hook into dd suite to perform some basic IO tests
source "$rootdir/test/dd/common.sh"

malloc_to_xnvme_copy() {
	# Use 1GB null_blk for the xnvme backend
	init_null_blk gb=1

	local mbdev0=malloc0 mbdev0_bs=512
	local xnvme0=null0 xnvme0_dev xnvme_io=()
	local io

	xnvme_io+=(libaio)
	xnvme_io+=(io_uring)

	# This always represents size of the device in 512B sectors
	# so it should align nicely with the $mbdev0_bs.
	mbdev0_b=$(< /sys/block/nullb0/size)
	xnvme0_dev=/dev/nullb0

	local -A method_bdev_malloc_create_0=(
		["name"]=$mbdev0
		["num_blocks"]=$mbdev0_b
		["block_size"]=$mbdev0_bs
	)

	local -A method_bdev_xnvme_create_0=()
	method_bdev_xnvme_create_0["name"]=$xnvme0
	method_bdev_xnvme_create_0["filename"]=$xnvme0_dev

	for io in "${xnvme_io[@]}"; do
		method_bdev_xnvme_create_0["io_mechanism"]="$io"

		"${DD_APP[@]}" \
			--ib="$mbdev0" \
			--ob="$xnvme0" \
			--json <(gen_conf)

		"${DD_APP[@]}" \
			--ib="$xnvme0" \
			--ob="$mbdev0" \
			--json <(gen_conf)
	done

	remove_null_blk
}

xnvme_bdevperf() {
	# Use 1GB null_blk for the xnvme backend
	init_null_blk gb=1

	local xnvme0=null0 xnvme0_dev xnvme_io=()
	local io

	xnvme_io+=(libaio)
	xnvme_io+=(io_uring)

	xnvme0_dev=/dev/nullb0

	local -A method_bdev_xnvme_create_0=()
	method_bdev_xnvme_create_0["name"]=$xnvme0
	method_bdev_xnvme_create_0["filename"]=$xnvme0_dev

	for io in "${xnvme_io[@]}"; do
		method_bdev_xnvme_create_0["io_mechanism"]="$io"
		"$rootdir/build/examples/bdevperf" \
			--json <(gen_conf) \
			-q 64 \
			-w randread \
			-t 5 \
			-T "$xnvme0" \
			-o 4096
	done

	remove_null_blk
}

run_test "xnvme_to_malloc_dd_copy" malloc_to_xnvme_copy
run_test "xnvme_bdevperf" xnvme_bdevperf
