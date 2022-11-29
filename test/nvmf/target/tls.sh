#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)

source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

nvmftestinit
nvmfappstart -m 0x2 --wait-for-rpc

if [ "$TEST_TRANSPORT" != tcp ]; then
	echo "Unsupported transport: $TEST_TRANSPORT"
	exit 0
fi

$rpc_py sock_set_default_impl -i ssl

# Check default TLS version
version=$($rpc_py sock_impl_get_options -i ssl | jq -r .tls_version)
if [[ "$version" != "0" ]]; then
	echo "TLS version was not set correctly $version != 0"
	exit 1
fi

# Check TLS version set to 13
$rpc_py sock_impl_set_options -i ssl --tls-version 13
version=$($rpc_py sock_impl_get_options -i ssl | jq -r .tls_version)
if [[ "$version" != "13" ]]; then
	echo "TLS version was not set correctly $version != 13"
	exit 1
fi

# Check TLS version set to 12
$rpc_py sock_impl_set_options -i ssl --tls-version 12
version=$($rpc_py sock_impl_get_options -i ssl | jq -r .tls_version)
if [[ "$version" != "12" ]]; then
	echo "TLS version was not set correctly $version != 12"
	exit 1
fi

# Check incorrect TLS version set to 7
$rpc_py sock_impl_set_options -i ssl --tls-version 7
version=$($rpc_py sock_impl_get_options -i ssl | jq -r .tls_version)
if [[ "$version" != "7" ]]; then
	echo "TLS version was not set correctly $version != 7"
	exit 1
fi

# Check default KTLS is disabled
ktls=$($rpc_py sock_impl_get_options -i ssl | jq -r .enable_ktls)
if [[ "$ktls" != "false" ]]; then
	echo "KTLS was not set correctly $ktls != false"
	exit 1
fi

# Check KTLS enable
$rpc_py sock_impl_set_options -i ssl --enable-ktls
ktls=$($rpc_py sock_impl_get_options -i ssl | jq -r .enable_ktls)
if [[ "$ktls" != "true" ]]; then
	echo "KTLS was not set correctly $ktls != true"
	exit 1
fi

# Check KTLS disable
$rpc_py sock_impl_set_options -i ssl --disable-ktls
ktls=$($rpc_py sock_impl_get_options -i ssl | jq -r .enable_ktls)
if [[ "$ktls" != "false" ]]; then
	echo "KTLS was not set correctly $ktls != false"
	exit 1
fi

# Check default PSK key
key=$($rpc_py sock_impl_get_options -i ssl | jq -r .psk_key)
if [[ "$key" != "null" ]]; then
	echo "TLS default key should be empty and not $key"
	exit 1
fi

# Check default PSK key set
$rpc_py sock_impl_set_options -i ssl --psk-key 1234567890ABCDEF
key=$($rpc_py sock_impl_get_options -i ssl | jq -r .psk_key)
if [[ "$key" != "1234567890ABCDEF" ]]; then
	echo "TLS key was not set correctly $key != 1234567890ABCDEF"
	exit 1
fi

# Check default PSK identity
identity=$($rpc_py sock_impl_get_options -i ssl | jq -r .psk_identity)
if [[ "$identity" != "null" ]]; then
	echo "TLS default identity should be empty and not $identity"
	exit 1
fi

# Check default PSK identity set
$rpc_py sock_impl_set_options -i ssl --psk-identity psk.spdk.io
identity=$($rpc_py sock_impl_get_options -i ssl | jq -r .psk_identity)
if [[ "$identity" != "psk.spdk.io" ]]; then
	echo "PSK ID was not set correctly $identity != psk.spdk.io"
	exit 1
fi

$rpc_py sock_impl_set_options -i ssl --tls-version 13
$rpc_py framework_start_init
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -m 10
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT \
	-a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py bdev_malloc_create 32 4096 -b malloc0
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 malloc0 -n 1

# Send IO
"${NVMF_TARGET_NS_CMD[@]}" $SPDK_EXAMPLE_DIR/perf -S ssl -q 64 -o 4096 -w randrw -M 30 -t 10 \
	-r "trtype:${TEST_TRANSPORT} adrfam:IPv4 traddr:${NVMF_FIRST_TARGET_IP} trsvcid:${NVMF_PORT} \
subnqn:nqn.2016-06.io.spdk:cnode1" --psk-key 1234567890ABCDEF --psk-identity psk.spdk.io

# use bdevperf to test "bdev_nvme_attach_controller"
bdevperf_rpc_sock=/var/tmp/bdevperf.sock
$rootdir/build/examples/bdevperf -m 0x4 -z -r $bdevperf_rpc_sock -q 128 -o 4096 -w verify -t 10 &
bdevperf_pid=$!

trap 'process_shm --id $NVMF_APP_SHM_ID; killprocess $bdevperf_pid; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid $bdevperf_rpc_sock
# send RPC
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b TLSTEST -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
	-s $NVMF_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1 --psk 1234567890ABCDEF
# run I/O and wait
$rootdir/examples/bdev/bdevperf/bdevperf.py -t 20 -s $bdevperf_rpc_sock perform_tests
# finish
killprocess $bdevperf_pid

trap - SIGINT SIGTERM EXIT
nvmftestfini
