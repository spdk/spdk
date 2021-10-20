#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")
source "$testdir/common.sh"

uring_zram_copy() {
	# Use zram for backend device - this is done in order to make the IO as fast
	# as possible.

	local zram_dev_id
	local magic
	local magic_file0=$SPDK_TEST_STORAGE/magic.dump0
	local magic_file1=$SPDK_TEST_STORAGE/magic.dump1
	local verify_magic

	init_zram
	zram_dev_id=$(create_zram_dev)
	set_zram_dev "$zram_dev_id" 512M

	local ubdev=uring0 ufile=/dev/zram$zram_dev_id

	local -A method_bdev_uring_create_0=(
		["filename"]=$ufile
		["name"]=$ubdev
	)

	# Add extra malloc bdev
	local mbdev=malloc0 mbdev_b=1048576 mbdev_bs=512

	local -A method_bdev_malloc_create_0=(
		["name"]=$mbdev
		["num_blocks"]=$mbdev_b
		["block_size"]=$mbdev_bs
	)

	magic=$(gen_bytes $((mbdev_bs * 2)))
	echo "$magic" > "$magic_file0"

	# Inflate the magic file to fill up entire zram of 512MB.
	"${DD_APP[@]}" \
		--if=/dev/zero \
		--of="$magic_file0" \
		--oflag=append \
		--bs=$((mbdev_b * mbdev_bs - ${#magic} - 1)) \
		--count=1

	# Copy magic file to uring bdev
	"${DD_APP[@]}" \
		--if="$magic_file0" \
		--ob="$ubdev" \
		--json <(gen_conf)

	# Copy the whole uring bdev back to a file
	"${DD_APP[@]}" \
		--ib="$ubdev" \
		--of="$magic_file1" \
		--json <(gen_conf)

	# Verify integrity of each copy
	read -rn${#magic} verify_magic < "$magic_file1"
	[[ $verify_magic == "$magic" ]]

	read -rn${#magic} verify_magic < "/dev/zram$zram_dev_id"
	[[ $verify_magic == "$magic" ]]

	diff -q "$magic_file0" "$magic_file1"

	# Copy cotents of uring bdev to malloc bdev
	"${DD_APP[@]}" \
		--ib="$ubdev" \
		--ob="$mbdev" \
		--json <(gen_conf)

	# HACK: small trick to utilize bdev_uring_delete and keep spdk_dd happy -
	# read/write from 0-length files.

	local -A method_bdev_uring_delete_0=(
		["name"]="$ubdev"
	)

	"${DD_APP[@]}" \
		--if=<(:) \
		--of=<(:) \
		--json <(gen_conf)

	# Now try to copy to uring bdev which is explicitly deleted. We expect it
	# to fail.

	NOT "${DD_APP[@]}" \
		--ib="$ubdev" \
		--of=<(:) \
		--json <(gen_conf)

	remove_zram_dev "$zram_dev_id"
	rm -f "$magic_file0" "$magic_file1"
}

run_test "dd_uring_copy" uring_zram_copy
