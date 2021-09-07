#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

rpc_py="$rootdir/scripts/rpc.py"

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512
NVMF_HOST_FIRST_PORT="60000"
NVMF_HOST_SECOND_PORT="60001"

bdevperf_rpc_sock=/var/tmp/bdevperf.sock

if [ "$TEST_TRANSPORT" == "rdma" ]; then
	echo "Skipping tests on RDMA because the rdma stack fails to configure the same IP for host and target."
	exit 0
fi

nvmftestinit

nvmfappstart -m 0xF

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc0

$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT

$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc1
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode2 -a -s SPDK00000000000002
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode2 Malloc1

$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode2 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode2 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT

$rootdir/test/bdev/bdevperf/bdevperf -z -r $bdevperf_rpc_sock -q 128 -o 4096 -w write -t 1 -f &> $testdir/try.txt &
bdevperf_pid=$!

trap 'process_shm --id $NVMF_APP_SHM_ID; pap "$testdir/try.txt"; killprocess $bdevperf_pid; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid $bdevperf_rpc_sock

# Create a controller from the first IP/Port combination.
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
	-s $NVMF_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1 -i $NVMF_FIRST_TARGET_IP -c $NVMF_HOST_FIRST_PORT

# wait for the first controller to show up.
while ! $rpc_py -s $bdevperf_rpc_sock bdev_nvme_get_controllers | grep -c NVMe; do
	((++bdev_nvme_get_controllers_timeout <= 10))
	sleep 1s
done

# try to attach with same controller name but different hostnqn. Should fail.
NOT $rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
	-s $NVMF_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1 -i $NVMF_FIRST_TARGET_IP -c $NVMF_HOST_FIRST_PORT \
	-q nqn.2021-09-7.io.spdk:00001

# try to attach with same controller name but different subnqn. Should fail.
NOT $rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
	-s $NVMF_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode2 -i $NVMF_FIRST_TARGET_IP -c $NVMF_HOST_FIRST_PORT

# try to attach with the same controller name and multipathing disabled. Should fail
NOT $rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
	-s $NVMF_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1 -i $NVMF_FIRST_TARGET_IP -c $NVMF_HOST_FIRST_PORT \
	-x disable

# Attempt to add an identical path as a failover path. Should fail.
NOT $rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
	-s $NVMF_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1 -i $NVMF_FIRST_TARGET_IP -c $NVMF_HOST_FIRST_PORT \
	-x failover

# Add a second path without specifying the host information. Should pass.
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
	-s $NVMF_SECOND_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1

# Remove the second path
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_detach_controller NVMe0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
	-s $NVMF_SECOND_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1

# Add a second controller by attaching to the same subsystem with a different controller name
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_attach_controller -b NVMe1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
	-s $NVMF_SECOND_PORT -f ipv4 -n nqn.2016-06.io.spdk:cnode1 -i $NVMF_FIRST_TARGET_IP -c $NVMF_HOST_FIRST_PORT

if [ "$($rpc_py -s $bdevperf_rpc_sock bdev_nvme_get_controllers | grep -c NVMe)" != "2" ]; then
	echo "actual number of controllers is not equal to expected count."
	exit 1
fi

$rootdir/test/bdev/bdevperf/bdevperf.py -s $bdevperf_rpc_sock perform_tests

# Remove the second controller
$rpc_py -s $bdevperf_rpc_sock bdev_nvme_detach_controller NVMe1

killprocess $bdevperf_pid

$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1
$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode2

trap - SIGINT SIGTERM EXIT

pap "$testdir/try.txt"
nvmftestfini
