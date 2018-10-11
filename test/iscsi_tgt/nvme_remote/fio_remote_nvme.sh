#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh
source $rootdir/test/iscsi_tgt/common.sh

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
if [ -z $NVMF_FIRST_TARGET_IP ]; then
	echo "no NIC for nvmf test"
	exit 0
fi

rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio.py"

NVMF_PORT=4420

# Namespaces are NOT used here on purpose. Rxe_cfg utilility used for NVMf tests do not support namespaces.
TARGET_IP=127.0.0.1
INITIATOR_IP=127.0.0.1
NETMASK=$INITIATOR_IP/32

function run_nvme_remote() {
	echo "now use $1 method to run iscsi tgt."

	# Start the iSCSI target without using stub
	iscsi_rpc_addr="/var/tmp/spdk-iscsi.sock"
	ISCSI_APP="$rootdir/app/iscsi_tgt/iscsi_tgt"
	$ISCSI_APP -r "$iscsi_rpc_addr" -m 0x1 -p 0 -s 512 --wait-for-rpc &
	iscsipid=$!
	echo "iSCSI target launched. pid: $iscsipid"
	trap "killprocess $iscsipid; killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT
	waitforlisten $iscsipid "$iscsi_rpc_addr"
	$rpc_py -s "$iscsi_rpc_addr" set_iscsi_options -o 30 -a 16
	$rpc_py -s "$iscsi_rpc_addr" start_subsystem_init
	if [ "$1" = "remote" ]; then
		$rpc_py -s $iscsi_rpc_addr construct_nvme_bdev -b "Nvme0" -t "rdma" -f "ipv4" -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -n nqn.2016-06.io.spdk:cnode1
	fi

	echo "iSCSI target has started."

	timing_exit start_iscsi_tgt

	echo "Creating an iSCSI target node."
	$rpc_py -s "$iscsi_rpc_addr" add_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
	$rpc_py -s "$iscsi_rpc_addr" add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
	if [ "$1" = "local" ]; then
		$rpc_py -s "$iscsi_rpc_addr" construct_nvme_bdev -b "Nvme0" -t "rdma" -f "ipv4" -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -n nqn.2016-06.io.spdk:cnode1
	fi
	$rpc_py -s "$iscsi_rpc_addr" construct_target_node Target1 Target1_alias 'Nvme0n1:0' $PORTAL_TAG:$INITIATOR_TAG 64 -d
	sleep 1

	echo "Logging in to iSCSI target."
	iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
	iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
}

timing_enter nvme_remote

# Start the NVMf target
NVMF_APP="$rootdir/app/nvmf_tgt/nvmf_tgt"
$NVMF_APP -m 0x2 -p 1 -s 512 --wait-for-rpc &
nvmfpid=$!
echo "NVMf target launched. pid: $nvmfpid"
trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT
waitforlisten $nvmfpid
$rpc_py start_subsystem_init
$rpc_py nvmf_create_transport -t RDMA -u 8192 -p 4
echo "NVMf target has started."
bdevs=$($rpc_py construct_malloc_bdev 64 512)
$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t rdma -a $NVMF_FIRST_TARGET_IP -s 4420
for bdev in $bdevs; do
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 $bdev
done
echo "NVMf subsystem created."

timing_enter start_iscsi_tgt

run_nvme_remote "local"

trap "iscsicleanup; killprocess $iscsipid; killprocess $nvmfpid; \
	rm -f ./local-job0-0-verify.state; exit 1" SIGINT SIGTERM EXIT
sleep 1

echo "Running FIO"
$fio_py 4096 1 randrw 1 verify

rm -f ./local-job0-0-verify.state
iscsicleanup
killprocess $iscsipid

run_nvme_remote "remote"

echo "Running FIO"
$fio_py 4096 1 randrw 1 verify

rm -f ./local-job0-0-verify.state
trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $iscsipid
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode1
killprocess $nvmfpid

report_test_completion "iscsi_nvme_remote"
timing_exit nvme_remote
