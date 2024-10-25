#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../../")
source "$testdir/common.sh"

xnvme_bdevperf() {
	local io_pattern
	local -n io_pattern_ref=$io

	for io_pattern in "${io_pattern_ref[@]}"; do
		"$rootdir/build/examples/bdevperf" \
			--json <(gen_conf) \
			-q 64 \
			-w "$io_pattern" \
			-t 5 \
			-T "$name" \
			-o 4096
	done
}

xnvme_fio_plugin() {
	local io_pattern
	local -n io_pattern_ref=${io}_fio

	for io_pattern in "${io_pattern_ref[@]}"; do
		fio_bdev \
			--ioengine=spdk_bdev \
			--spdk_json_conf=<(gen_conf) \
			--filename="$name" \
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
}

xnvme_rpc() {
	local -A cc=()

	cc["false"]="" cc["true"]="-c"

	"${SPDK_APP[@]}" &
	spdk_tgt=$!
	waitforlisten "$spdk_tgt"

	rpc_cmd bdev_xnvme_create \
		"$filename" \
		"$name" \
		"$io" \
		"${cc["$conserve_cpu"]}"

	[[ $(rpc_xnvme "name") == "$name" ]]
	[[ $(rpc_xnvme "filename") == "$filename" ]]
	[[ $(rpc_xnvme "io_mechanism") == "$io" ]]
	[[ $(rpc_xnvme "conserve_cpu") == "$conserve_cpu" ]]

	rpc_cmd bdev_xnvme_delete \
		"$name"

	killprocess "$spdk_tgt"
}

trap 'killprocess "$spdk_tgt"' EXIT

for io in "${xnvme_io[@]}"; do
	method_bdev_xnvme_create_0["io_mechanism"]=$io
	method_bdev_xnvme_create_0["filename"]=${xnvme_filename["$io"]}

	filename=${method_bdev_xnvme_create_0["filename"]}
	name=${method_bdev_xnvme_create_0["name"]}

	for cc in "${xnvme_conserve_cpu[@]}"; do
		method_bdev_xnvme_create_0["conserve_cpu"]=$cc
		conserve_cpu=${method_bdev_xnvme_create_0["conserve_cpu"]}

		run_test "xnvme_rpc" xnvme_rpc
		run_test "xnvme_bdevperf" xnvme_bdevperf
		run_test "xnvme_fio_plugin" xnvme_fio_plugin
	done
done
