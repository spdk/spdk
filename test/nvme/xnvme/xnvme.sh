#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../../")
# Hook into dd suite to perform some basic IO tests
source "$rootdir/test/dd/common.sh"

malloc_to_xnvme_copy() {
	# Use zram for the xnvme backend
	init_zram

	local mbdev0=malloc0 mbdev0_b=1048576 mbdev0_bs=512
	local xnvme0=zram0 xnvme0_dev xnvme_io=()
	local io

	xnvme_io+=(libaio)
	xnvme_io+=(io_uring)

	xnvme0_dev=$(create_zram_dev)
	set_zram_dev "$xnvme0_dev" 512M

	local -A method_bdev_malloc_create_0=(
		["name"]=$mbdev0
		["num_blocks"]=$mbdev0_b
		["block_size"]=$mbdev0_bs
	)

	local -A method_bdev_xnvme_create_0=()
	method_bdev_xnvme_create_0["name"]=$xnvme0
	method_bdev_xnvme_create_0["filename"]="/dev/zram$xnvme0_dev"

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

	remove_zram_dev "$xnvme0_dev"
}

xnvme_bdevperf() {
	# Use zram for the xnvme backend
	init_zram

	local xnvme0=zram0 xnvme0_dev xnvme_io=()
	local io

	xnvme_io+=(libaio)
	xnvme_io+=(io_uring)

	xnvme0_dev=$(create_zram_dev)
	set_zram_dev "$xnvme0_dev" 512M

	local -A method_bdev_xnvme_create_0=()
	method_bdev_xnvme_create_0["name"]=$xnvme0
	method_bdev_xnvme_create_0["filename"]="/dev/zram$xnvme0_dev"

	for io in "${xnvme_io[@]}"; do
		method_bdev_xnvme_create_0["io_mechanism"]="$io"
		"$rootdir/test/bdev/bdevperf/bdevperf" \
			--json <(gen_conf) \
			-q 64 \
			-w randread \
			-t 5 \
			-T "$xnvme0" \
			-o 4096
	done

	remove_zram_dev "$xnvme0_dev"
}

run_test "xnvme_to_malloc_dd_copy" malloc_to_xnvme_copy
run_test "xnvme_bdevperf" xnvme_bdevperf
