#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation
#  All rights reserved.
#
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
	local corpus_dir
	corpus_dir=/tmp/llvm_fuzz$fuzzer_type
	mkdir -p $corpus_dir
	$rootdir/test/app/fuzz/llvm_nvme_fuzz/llvm_nvme_fuzz -m 0x1 -i 0 -F "$trid" -c $testdir/fuzz_json.conf -t $TIME -D $corpus_dir -Z $fuzzer_type
}

function run_fuzz() {
	local startday
	local today
	local interval=0
	local weekloop
	# Get the date number, format is like '22078'
	# The purpose is when Jenkins schedule one fuzz in Saturday
	# We can decide which one fuzz will be run , there are lots of fuzz, but only run one of them in Saturday each time
	# and make sure all fuzz will be tested, so use this function. Such run fuzz 0 in 03/26, and run fuzz 1 in 04/02, run fuzz 2 in 04/09 ....
	startday=$(date -d '2022-03-19' '+%y%j')
	today=$(date '+%y%j')
	interval=$(((today - startday) / 7))
	weekloop=$((interval / fuzz_num))
	if [[ $weekloop -lt 1 ]]; then # The first loop of fuzz
		fuzzer_type=$interval
	else
		fuzzer_type=$((interval % fuzz_num))
	fi
	start_llvm_fuzz $fuzzer_type &> $output_dir/fuzzer_${fuzzer_type}.log
}

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../../../)
source $rootdir/test/common/autotest_common.sh

fuzzfile=$rootdir/test/app/fuzz/llvm_nvme_fuzz/llvm_nvme_fuzz.c
fuzz_num=$(($(grep -c "fn =" $fuzzfile) - 1))
[[ $fuzz_num -ne 0 ]]

trap 'process_shm --id 0; rm -rf /tmp/llvm_fuzz*; exit 1' SIGINT SIGTERM EXIT

trid="trtype:tcp adrfam:IPv4 subnqn:nqn.2016-06.io.spdk:cnode1 traddr:127.0.0.1 trsvcid:4420"

if [[ $SPDK_TEST_FUZZER_SHORT -eq 1 ]]; then
	for ((i = 0; i < fuzz_num; i++)); do
		start_llvm_fuzz $i
	done
elif [[ $SPDK_TEST_FUZZER -eq 1 ]]; then
	run_fuzz
else
	start_llvm_fuzz $1
fi

rm -rf /tmp/llvm_fuzz*
trap - SIGINT SIGTERM EXIT
