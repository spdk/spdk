#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../../")
# Hook into dd suite to perform some basic IO tests
source "$rootdir/test/dd/common.sh"
source "$rootdir/test/common/nvme/functions.sh"

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
	local io io_pattern

	xnvme_io+=(libaio)
	xnvme_io+=(io_uring)
	xnvme_io+=(io_uring_cmd)

	xnvme0_dev=/dev/nullb0

	local -A method_bdev_xnvme_create_0=()
	method_bdev_xnvme_create_0["name"]=$xnvme0
	method_bdev_xnvme_create_0["filename"]=$xnvme0_dev

	for io in "${xnvme_io[@]}"; do
		if [[ $io == io_uring_cmd ]]; then
			# This can work only with generic nvme devices (char) so
			# fail hard if it's not around.
			[[ -n ${ng0n1[*]} ]]
			method_bdev_xnvme_create_0["filename"]=/dev/ng0n1
		fi
		method_bdev_xnvme_create_0["io_mechanism"]="$io"
		local -n io_pattern_ref=$io
		for io_pattern in "${io_pattern_ref[@]}"; do
			"$rootdir/build/examples/bdevperf" \
				--json <(gen_conf) \
				-q 64 \
				-w "$io_pattern" \
				-t 5 \
				-T "$xnvme0" \
				-o 4096
		done
	done

	remove_null_blk
}

xnvme_fio_plugin() {
	# Use 1GB null_blk for the xnvme backend
	init_null_blk gb=1

	local xnvme0=null0 xnvme0_dev xnvme_io=()
	local io io_pattern

	xnvme_io+=(libaio)
	xnvme_io+=(io_uring)
	xnvme_io+=(io_uring_cmd)

	xnvme0_dev=/dev/nullb0

	local -A method_bdev_xnvme_create_0=()
	method_bdev_xnvme_create_0["name"]=$xnvme0
	method_bdev_xnvme_create_0["filename"]=$xnvme0_dev
	method_bdev_xnvme_create_0["conserve_cpu"]=true

	for io in "${xnvme_io[@]}"; do
		if [[ $io == io_uring_cmd ]]; then
			# This can work only with generic nvme devices (char) so
			# fail hard if it's not around.
			[[ -n ${ng0n1[*]} ]]
			method_bdev_xnvme_create_0["filename"]=/dev/ng0n1
		fi
		method_bdev_xnvme_create_0["io_mechanism"]="$io"
		local -n io_pattern_ref=${io}_fio
		for io_pattern in "${io_pattern_ref[@]}"; do
			fio_bdev \
				--ioengine=spdk_bdev \
				--spdk_json_conf=<(gen_conf) \
				--filename="$xnvme0" \
				--direct=1 \
				--bs=4k \
				--iodepth=64 \
				--numjobs=1 \
				--rw="$io_pattern" \
				--time_based \
				--runtime=5 \
				--thread=1 \
				--name "xnvme_bdev"
		done
	done
}

xnvme_rpc() {
	local xnvme0=null0 xnvme0_dev xnvme_io=()
	local io flip=0 cc=() cc_b=()

	"${SPDK_APP[@]}" &
	spdk_tgt=$!
	waitforlisten "$spdk_tgt"

	# Use 1GB null_blk for the xnvme backend
	init_null_blk gb=1

	xnvme_io+=(libaio)
	xnvme_io+=(io_uring)
	xnvme_io+=(io_uring_cmd)

	cc[0]="" cc[1]="-c"
	cc_b[0]=false cc_b[1]=true

	xnvme0_dev=/dev/nullb0

	for io in "${xnvme_io[@]}"; do
		flip=$((!flip))

		rpc_cmd bdev_xnvme_create \
			"$xnvme0_dev" \
			"$xnvme0" \
			"$io" \
			"${cc[flip]}"

		[[ $(rpc_xnvme "name") == "$xnvme0" ]]
		[[ $(rpc_xnvme "filename") == "$xnvme0_dev" ]]
		[[ $(rpc_xnvme "io_mechanism") == "$io" ]]
		[[ $(rpc_xnvme "conserve_cpu") == "${cc_b[flip]}" ]]

		rpc_cmd bdev_xnvme_delete \
			"$xnvme0"
	done

	killprocess "$spdk_tgt"
}

rpc_xnvme() {
	rpc_cmd framework_get_config bdev \
		| jq -r ".[] | select(.method == \"bdev_xnvme_create\").params.${1:-name}"
}

trap 'killprocess "$spdk_tgt"' EXIT

# Prep global refs for io_pattern supported per io_mechanism
libaio=(randread randwrite)
io_uring=(randread randwrite)
io_uring_cmd=(randread randwrite unmap write_zeroes)
libaio_fio=("${libaio[@]}")
io_uring_fio=("${io_uring[@]}")
io_uring_cmd_fio=("${io_uring_fio[@]}")

"$rootdir/scripts/setup.sh" reset

scan_nvme_ctrls

run_test "xnvme_rpc" xnvme_rpc
run_test "xnvme_to_malloc_dd_copy" malloc_to_xnvme_copy
run_test "xnvme_bdevperf" xnvme_bdevperf
run_test "xnvme_fio_plugin" xnvme_fio_plugin

"$rootdir/scripts/setup.sh"
