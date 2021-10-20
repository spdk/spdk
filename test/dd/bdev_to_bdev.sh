#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")
source "$testdir/common.sh"

nvmes=("$@")

offset_magic() {
	local magic_check
	local offsets offset

	offsets=(16 64) # * bs

	for offset in "${offsets[@]}"; do
		"${DD_APP[@]}" \
			--ib="$bdev0" \
			--ob="$bdev1" \
			--count="$count" \
			--seek="$offset" \
			--bs="$bs" \
			--json <(gen_conf)

		"${DD_APP[@]}" \
			--ib="$bdev1" \
			--of="$test_file1" \
			--count=1 \
			--skip="$offset" \
			--bs="$bs" \
			--json <(gen_conf)

		read -rn${#magic} magic_check < "$test_file1"
		[[ $magic_check == "$magic" ]]
	done
}

cleanup() {
	# Zero up to 64M on input|output bdev
	clear_nvme "$bdev0" "" $((0x400000 + ${#magic}))
	clear_nvme "$bdev1" "" $((0x400000 + ${#magic}))
	rm -f "$test_file0" "$test_file1" "$aio1"
}

trap "cleanup" EXIT

bs=$((1024 << 10))

if ((${#nvmes[@]} > 1)); then
	nvme0=Nvme0 bdev0=Nvme0n1 nvme0_pci=${nvmes[0]} # input bdev
	nvme1=Nvme1 bdev1=Nvme1n1 nvme1_pci=${nvmes[1]} # output bdev

	declare -A method_bdev_nvme_attach_controller_0=(
		["name"]=$nvme0
		["traddr"]=$nvme0_pci
		["trtype"]=pcie
	)
	declare -A method_bdev_nvme_attach_controller_1=(
		["name"]=$nvme1
		["traddr"]=$nvme1_pci
		["trtype"]=pcie
	)
else
	# Use AIO to compensate lack of actual hardware
	nvme0=Nvme0 bdev0=Nvme0n1 nvme0_pci=${nvmes[0]} # input bdev
	aio1=$SPDK_TEST_STORAGE/aio1 bdev1=aio1         # output bdev

	declare -A method_bdev_nvme_attach_controller_1=(
		["name"]=$nvme0
		["traddr"]=$nvme0_pci
		["trtype"]=pcie
	)
	declare -A method_bdev_aio_create_0=(
		["name"]=$bdev1
		["filename"]=$aio1
		["block_size"]=4096
	)

	# 256MB AIO file
	"${DD_APP[@]}" \
		--if=/dev/zero \
		--of="$aio1" \
		--bs="$bs" \
		--count=256
fi

test_file0=$SPDK_TEST_STORAGE/dd.dump0
test_file1=$SPDK_TEST_STORAGE/dd.dump1

magic="This Is Our Magic, find it"
echo "$magic" > "$test_file0"

# Make the file a bit bigger (~64MB)
run_test "dd_inflate_file" \
	"${DD_APP[@]}" \
	--if=/dev/zero \
	--of="$test_file0" \
	--oflag=append \
	--bs="$bs" \
	--count=64

test_file0_size=$(wc -c < "$test_file0")

# Now, copy it over to first nvme with default bs (4k)
run_test "dd_copy_to_out_bdev" \
	"${DD_APP[@]}" \
	--if="$test_file0" \
	--ob="$bdev0" \
	--json <(gen_conf)

count=$(((test_file0_size / bs) + 1))

run_test "dd_offset_magic" offset_magic
