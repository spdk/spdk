#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

NVMF_EXAMPLE=("$SPDK_EXAMPLE_DIR/nvmf")

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

function build_nvmf_example_args() {
	if [ $SPDK_RUN_NON_ROOT -eq 1 ]; then
		NVMF_EXAMPLE=(sudo -u "$USER" "${NVMF_EXAMPLE[@]}")
		NVMF_EXAMPLE+=(-i "$NVMF_APP_SHM_ID" -g 10000)
	else
		NVMF_EXAMPLE+=(-i "$NVMF_APP_SHM_ID" -g 10000)
	fi
}

build_nvmf_example_args

function nvmfexamplestart() {
	timing_enter start_nvmf_example

	if [ "$TEST_TRANSPORT" == "tcp" ]; then
		NVMF_EXAMPLE=("${NVMF_TARGET_NS_CMD[@]}" "${NVMF_EXAMPLE[@]}")
	fi

	"${NVMF_EXAMPLE[@]}" $1 &
	nvmfpid=$!
	trap 'process_shm --id $NVMF_APP_SHM_ID; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $nvmfpid
	timing_exit start_nvmf_example
}

timing_enter nvmf_example_test
nvmftestinit
nvmfexamplestart "-m 0xF"

#create transport
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
#create malloc bdev
malloc_bdevs="$($rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE) "
#create subsystem
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001

#add ns to subsystem
for malloc_bdev in $malloc_bdevs; do
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 "$malloc_bdev"
done

#add listener to subsystem
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

perf="$SPDK_EXAMPLE_DIR/perf"

$perf -q 64 -o 4096 -w randrw -M 30 -t 10 \
	-r "trtype:${TEST_TRANSPORT} adrfam:IPv4 traddr:${NVMF_FIRST_TARGET_IP} trsvcid:${NVMF_PORT} \
subnqn:nqn.2016-06.io.spdk:cnode1"

trap - SIGINT SIGTERM EXIT
nvmftestfini
timing_exit nvmf_example_test
