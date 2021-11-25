#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh
source $rootdir/test/iscsi_tgt/common.sh

nvmftestinit
iscsitestinit

rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio-wrapper"

# Namespaces are NOT used here on purpose. Rxe_cfg utility used for NVMf tests do not support namespaces.
TARGET_IP=127.0.0.1
INITIATOR_IP=127.0.0.1
NETMASK=$INITIATOR_IP/32

function run_nvme_remote() {
	echo "now use $1 method to run iscsi tgt."

	iscsi_rpc_addr="/var/tmp/spdk-iscsi.sock"
	"${ISCSI_APP[@]}" -r "$iscsi_rpc_addr" -m 0x1 -p 0 -s 512 --wait-for-rpc &
	iscsipid=$!
	echo "iSCSI target launched. pid: $iscsipid"
	trap 'killprocess $iscsipid; iscsitestfini; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $iscsipid "$iscsi_rpc_addr"
	$rpc_py -s "$iscsi_rpc_addr" iscsi_set_options -o 30 -a 16
	$rpc_py -s "$iscsi_rpc_addr" framework_start_init
	if [ "$1" = "remote" ]; then
		$rpc_py -s $iscsi_rpc_addr bdev_nvme_attach_controller -b "Nvme0" -t "rdma" -f "ipv4" -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -n nqn.2016-06.io.spdk:cnode1
	fi

	echo "iSCSI target has started."

	timing_exit start_iscsi_tgt

	echo "Creating an iSCSI target node."
	$rpc_py -s "$iscsi_rpc_addr" iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
	$rpc_py -s "$iscsi_rpc_addr" iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
	if [ "$1" = "local" ]; then
		$rpc_py -s "$iscsi_rpc_addr" bdev_nvme_attach_controller -b "Nvme0" -t "rdma" -f "ipv4" -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -n nqn.2016-06.io.spdk:cnode1
	fi
	$rpc_py -s "$iscsi_rpc_addr" iscsi_create_target_node Target1 Target1_alias 'Nvme0n1:0' $PORTAL_TAG:$INITIATOR_TAG 64 -d
	sleep 1

	echo "Logging in to iSCSI target."
	iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
	iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
}

# Start the NVMf target
"${NVMF_APP[@]}" -m 0x2 -p 1 -s 512 --wait-for-rpc &
nvmfpid=$!
echo "NVMf target launched. pid: $nvmfpid"
trap 'iscsitestfini; nvmftestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten $nvmfpid
$rpc_py framework_start_init
$rpc_py nvmf_create_transport -t RDMA -u 8192
echo "NVMf target has started."
bdevs=$($rpc_py bdev_malloc_create 64 512)
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t rdma -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
for bdev in $bdevs; do
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 $bdev
done
echo "NVMf subsystem created."

timing_enter start_iscsi_tgt

run_nvme_remote "local"

trap 'iscsicleanup; killprocess $iscsipid;
	rm -f ./local-job0-0-verify.state; iscsitestfini; nvmftestfini; exit 1' SIGINT SIGTERM EXIT

echo "Running FIO"
$fio_py -p iscsi -i 4096 -d 1 -t randrw -r 1 -v

rm -f ./local-job0-0-verify.state
iscsicleanup
killprocess $iscsipid

run_nvme_remote "remote"

echo "Running FIO"
$fio_py -p iscsi -i 4096 -d 1 -t randrw -r 1 -v

rm -f ./local-job0-0-verify.state
trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $iscsipid
$rpc_py nvmf_delete_subsystem nqn.2016-06.io.spdk:cnode1

iscsitestfini
nvmftestfini
