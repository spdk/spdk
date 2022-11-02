#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  All rights reserved.
#

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")
source "$testdir/common.sh"

cleanup() {
	rm $aio_disk
	rm $file1
	rm $file2
	rm $file3
}

prepare() {
	truncate $aio_disk --size 104857600

	dd if=/dev/zero of=$file1 bs=4M count=1
	dd if=/dev/zero of=$file1 bs=4M count=1 seek=4
	dd if=/dev/zero of=$file1 bs=4M count=1 seek=8
}

file_to_file() {
	local stat1_s stat1_b
	local stat2_s stat2_b

	local -A method_bdev_aio_create_0=(
		["filename"]=$aio_disk
		["name"]=$aio_bdev
		["block_size"]=4096
	)

	local -A method_bdev_lvol_create_lvstore_1=(
		["bdev_name"]=$aio_bdev
		["lvs_name"]=$lvstore
	)

	"${DD_APP[@]}" \
		--if="$file1" \
		--of="$file2" \
		--bs=12582912 \
		--sparse \
		--json <(gen_conf)

	stat1_s=$(stat --printf='%s' $file1)
	stat2_s=$(stat --printf='%s' $file2)

	[[ $stat1_s == "$stat2_s" ]]

	stat1_b=$(stat --printf='%b' $file1)
	stat2_b=$(stat --printf='%b' $file2)

	[[ $stat1_b == "$stat2_b" ]]
}

file_to_bdev() {
	local -A method_bdev_aio_create_0=(
		["filename"]=$aio_disk
		["name"]=$aio_bdev
		["block_size"]=4096
	)

	local -A method_bdev_lvol_create_1=(
		["lvs_name"]=$lvstore
		["lvol_name"]=$lvol
		["size"]=37748736
		["thin_provision"]=true
	)

	"${DD_APP[@]}" \
		--if="$file2" \
		--ob="$lvstore/$lvol" \
		--bs=12582912 \
		--sparse \
		--json <(gen_conf)
}

bdev_to_file() {
	local stat2_s stat2_b
	local stat3_s stat3_b

	local -A method_bdev_aio_create_0=(
		["filename"]=$aio_disk
		["name"]=$aio_bdev
		["block_size"]=4096
	)

	"${DD_APP[@]}" \
		--ib="$lvstore/$lvol" \
		--of="$file3" \
		--bs=12582912 \
		--sparse \
		--json <(gen_conf)

	stat2_s=$(stat --printf='%s' $file2)
	stat3_s=$(stat --printf='%s' $file3)

	[[ $stat2_s == "$stat3_s" ]]

	stat2_b=$(stat --printf='%b' $file2)
	stat3_b=$(stat --printf='%b' $file3)

	[[ $stat2_b == "$stat3_b" ]]
}

aio_disk="dd_sparse_aio_disk"
aio_bdev="dd_aio"
file1="file_zero1"
file2="file_zero2"
file3="file_zero3"
lvstore="dd_lvstore"
lvol="dd_lvol"

trap "cleanup" EXIT

prepare

run_test "dd_sparse_file_to_file" file_to_file
run_test "dd_sparse_file_to_bdev" file_to_bdev
run_test "dd_sparse_bdev_to_file" bdev_to_file
