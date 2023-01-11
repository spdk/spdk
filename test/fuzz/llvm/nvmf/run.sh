#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation
#  All rights reserved.
#

FUZZER=nvmf
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
	local corpus_dir=$rootdir/../corpus/llvm_nvmf_$fuzzer_type
	local nvmf_cfg=/tmp/fuzz_json_$fuzzer_type.conf

	port="44$(printf "%02d" $fuzzer_type)"
	mkdir -p $corpus_dir

	trid="trtype:tcp adrfam:IPv4 subnqn:nqn.2016-06.io.spdk:cnode1 traddr:127.0.0.1 trsvcid:$port"
	sed -e "s/\"trsvcid\": \"4420\"/\"trsvcid\": \"$port\"/" $testdir/fuzz_json.conf > $nvmf_cfg

	$rootdir/test/app/fuzz/llvm_nvme_fuzz/llvm_nvme_fuzz \
		-m $core \
		-s $mem_size \
		-F "$trid" \
		-c $nvmf_cfg \
		-t $timen \
		-D $corpus_dir \
		-Z $fuzzer_type \
		-r /var/tmp/spdk$fuzzer_type.sock

	rm -rf $nvmf_cfg
}

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../../../)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/setup/common.sh
source $testdir/../common.sh

fuzzfile=$rootdir/test/app/fuzz/llvm_nvme_fuzz/llvm_nvme_fuzz.c
fuzz_num=$(($(grep -c "\.fn =" $fuzzfile) - 1))
((fuzz_num != 0))

trap 'cleanup /tmp/llvm_fuzz*; exit 1' SIGINT SIGTERM EXIT

mem_size=512
if [[ $SPDK_TEST_FUZZER_SHORT -eq 1 ]]; then
	start_llvm_fuzz_short $fuzz_num $TIME
elif [[ $SPDK_TEST_FUZZER -eq 1 ]]; then
	get_testn $fuzz_num $mem_size
	start_llvm_fuzz_all $TESTN $fuzz_num $TIME
else
	start_llvm_fuzz $1 $TIME 0x1
fi

trap - SIGINT SIGTERM EXIT
