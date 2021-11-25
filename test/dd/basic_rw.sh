#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")
source "$testdir/common.sh"

basic_rw() {
	local native_bs=$1
	local count size
	local qds bss

	qds=(1 64)
	# Generate some bs for tests based on the native_bs
	for bs in {0..2}; do
		bss+=($((native_bs << bs)))
	done

	for bs in "${bss[@]}"; do
		for qd in "${qds[@]}"; do
			count=$((0xffff / bs))
			count=$((count == 0 ? 1 : count))
			size=$((count * bs))

			gen_bytes "$size" > "$test_file0"

			"${DD_APP[@]}" \
				--if="$test_file0" \
				--ob="$bdev0" \
				--bs="$bs" \
				--qd="$qd" \
				--json <(gen_conf)

			"${DD_APP[@]}" \
				--ib="$bdev0" \
				--of="$test_file1" \
				--bs="$bs" \
				--qd="$qd" \
				--count="$count" \
				--json <(gen_conf)

			diff -q "$test_file0" "$test_file1"
			clear_nvme "$bdev0" "" "$size"
		done
	done
}

basic_offset() {
	# Check if offsetting works - using default io size of 4k
	local count seek skip data data_check

	gen_bytes 4096 > "$test_file0"
	((count = seek = skip = 1))
	data=$(< "$test_file0")

	"${DD_APP[@]}" \
		--if="$test_file0" \
		--ob="$bdev0" \
		--seek="$seek" \
		--json <(gen_conf)

	"${DD_APP[@]}" \
		--ib="$bdev0" \
		--of="$test_file1" \
		--skip="$skip" \
		--count="$count" \
		--json <(gen_conf)

	read -rn${#data} data_check < "$test_file1"
	[[ $data == "$data_check" ]]
}

cleanup() {
	clear_nvme "$bdev0"
	rm -f "$test_file0" "$test_file1"
}

trap "cleanup" EXIT

nvmes=("$@")
nvme0=Nvme0 nvme0_pci=${nvmes[0]} bdev0=Nvme0n1

declare -A method_bdev_nvme_attach_controller_0=(
	["name"]=$nvme0
	["traddr"]=$nvme0_pci
	["trtype"]=pcie
)

test_file0=$SPDK_TEST_STORAGE/dd.dump0
test_file1=$SPDK_TEST_STORAGE/dd.dump1
native_bs=$(get_native_nvme_bs "$nvme0_pci")

# Test if running with bs < native_bs successfully fails
run_test "dd_bs_lt_native_bs" \
	NOT "${DD_APP[@]}" \
	--if=<(:) \
	--ob="$bdev0" \
	--bs=$((native_bs >> 1)) \
	--json <(gen_conf)

run_test "dd_rw" basic_rw "$native_bs"
run_test "dd_rw_offset" basic_offset
