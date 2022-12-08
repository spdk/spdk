#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#
FUZZER=vfio
if [[ $SPDK_TEST_FUZZER_SHORT -eq 0 ]]; then
	TIME=60000
else
	TIME=1
fi

for i in "$@"; do
	case "$i" in
		--time=*)
			TIME="${i#*=}"
			;;
	esac
done

function start_llvm_fuzz() {
	local fuzzer_type=$1
	local timen=$2
	local core=$3
	local corpus_dir=$rootdir/../corpus/llvm_vfio_$fuzzer_type
	local fuzzer_dir=/tmp/vfio-user-$fuzzer_type
	local vfiouser_dir=$fuzzer_dir/domain/1
	local vfiouser_io_dir=$fuzzer_dir/domain/2
	local vfiouser_cfg=$fuzzer_dir/fuzz_vfio_json.conf

	mkdir -p $fuzzer_dir $vfiouser_dir $vfiouser_io_dir $corpus_dir

	# Adjust paths to allow multiply instance of fuzzer
	sed -e "s%/tmp/vfio-user/domain/1%$vfiouser_dir%;
		s%/tmp/vfio-user/domain/2%$vfiouser_io_dir%" $testdir/fuzz_vfio_json.conf > $vfiouser_cfg

	$rootdir/test/app/fuzz/llvm_vfio_fuzz/llvm_vfio_fuzz \
		-m $core \
		-s $mem_size \
		-P $output_dir/llvm/ \
		-F $vfiouser_dir \
		-c $vfiouser_cfg \
		-t $timen \
		-D $corpus_dir \
		-Y $vfiouser_io_dir \
		-r $fuzzer_dir/spdk$fuzzer_type.sock \
		-Z $fuzzer_type

	rm -rf $fuzzer_dir
}

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../../../)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/setup/common.sh
source $testdir/../common.sh

fuzzfile=$rootdir/test/app/fuzz/llvm_vfio_fuzz/llvm_vfio_fuzz.c
fuzz_num=$(($(grep -c "\.fn =" $fuzzfile) - 1))
((fuzz_num != 0))

trap 'cleanup /tmp/vfio-user-*; exit 1' SIGINT SIGTERM EXIT

# vfiouser transport is unable to connect if memory is restricted
mem_size=0
if [[ $SPDK_TEST_FUZZER_SHORT -eq 1 ]]; then
	start_llvm_fuzz_short $fuzz_num $TIME
elif [[ $SPDK_TEST_FUZZER -eq 1 ]]; then
	get_testn $fuzz_num 1024
	start_llvm_fuzz_all $TESTN $fuzz_num $TIME
else
	start_llvm_fuzz $1 $TIME 0x1
fi

trap - SIGINT SIGTERM EXIT
