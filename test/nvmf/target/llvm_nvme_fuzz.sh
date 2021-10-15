#!/usr/bin/env bash

TIME=10
FUZZER=0
for i in "$@"; do
	case "$i" in
		--time=*)
			TIME="${i#*=}"
			;;
		--fuzzer=*)
			FUZZER="${i#*=}"
			;;
	esac
done

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh

trap 'process_shm --id 0; exit 1' SIGINT SIGTERM EXIT

mkdir -p /tmp/corpus$FUZZER
trid="trtype:tcp adrfam:IPv4 subnqn:nqn.2016-06.io.spdk:cnode1 traddr:127.0.0.1 trsvcid:4420"

$rootdir/test/app/fuzz/llvm_nvme_fuzz/llvm_nvme_fuzz -m 0x1 -i 0 -F "$trid" -c $testdir/fuzz_json.conf -t $TIME -D /tmp/corpus$FUZZER -Z $FUZZER

trap - SIGINT SIGTERM EXIT
