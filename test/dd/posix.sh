#!/usr/bin/env bash
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")
source "$testdir/common.sh"

cleanup() {
	rm -f "$test_file0"{,.link}
	rm -f "$test_file1"{,.link}
}

append() {
	local dump0
	local dump1

	dump0=$(gen_bytes 32)
	dump1=$(gen_bytes 32)

	printf '%s' "$dump0" > "$test_file0"
	printf '%s' "$dump1" > "$test_file1"

	"${DD_APP[@]}" --if="$test_file0" --of="$test_file1" --oflag=append

	[[ $(< "$test_file1") == "${dump1}${dump0}" ]]
}

directory() {
	NOT "${DD_APP[@]}" --if="$test_file0" --iflag=directory --of="$test_file0"
	NOT "${DD_APP[@]}" --if="$test_file0" --of="$test_file0" --oflag=directory
}

nofollow() {
	local test_file0_link=$test_file0.link
	local test_file1_link=$test_file1.link

	ln -fs "$test_file0" "$test_file0_link"
	ln -fs "$test_file1" "$test_file1_link"

	NOT "${DD_APP[@]}" --if="$test_file0_link" --iflag=nofollow --of="$test_file1"
	NOT "${DD_APP[@]}" --if="$test_file0" --of="$test_file1_link" --oflag=nofollow

	# Do an extra step of checking if we actually can follow symlinks
	gen_bytes 512 > "$test_file0"

	"${DD_APP[@]}" --if="$test_file0_link" --of="$test_file1"
	[[ $(< "$test_file0") == "$(< "$test_file1")" ]]
}

noatime() {
	local atime_if
	local atime_of

	# It seems like spdk_dd doesn't update the atime in case 0 bytes are copied.
	# This differs from how standard dd works for instance
	gen_bytes 512 > "$test_file0"

	atime_if=$(stat --printf="%X" "$test_file0")
	atime_of=$(stat --printf="%X" "$test_file1")

	"${DD_APP[@]}" --if="$test_file0" --iflag=noatime --of="$test_file1"
	((atime_if == $(stat --printf="%X" "$test_file0")))
	((atime_of == $(stat --printf="%X" "$test_file1")))

	"${DD_APP[@]}" --if="$test_file0" --of="$test_file1"
	((atime_if < $(stat --printf="%X" "$test_file0")))
}

io() {
	local flags_ro flags_rw flag_ro flag_rw

	# O_NONBLOCK is actually a no-op, from a functional perspective, while
	# open()ing a regular file, but let's keep it just to test its usage.
	flags_ro=(direct nonblock)
	flags_rw=("${flags_ro[@]}" sync dsync)

	# simply check if data was correctly copied between files
	for flag_ro in "${flags_ro[@]}"; do
		gen_bytes 512 > "$test_file0"
		for flag_rw in "${flags_rw[@]}"; do
			"${DD_APP[@]}" \
				--if="$test_file0" \
				--iflag="$flag_ro" \
				--of="$test_file1" \
				--oflag="$flag_rw"
			[[ $(< "$test_file0") == "$(< "$test_file1")" ]]
		done
	done
}

trap "cleanup" EXIT

test_file0=$SPDK_TEST_STORAGE/dd.dump0
test_file1=$SPDK_TEST_STORAGE/dd.dump1

run_test "dd_flag_append" append
run_test "dd_flag_directory" directory
run_test "dd_flag_nofollow" nofollow
run_test "dd_flag_noatime" noatime
run_test "dd_flags_misc" io
