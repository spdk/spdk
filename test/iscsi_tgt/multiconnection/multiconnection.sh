#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

TARGET_IP=127.0.0.1
INITIATOR_IP=127.0.0.1
ISCSI_PORT=3260

lvol_bdevs=()

set -e

function linux_iter_pci {
	lspci -mm -n -D | grep $1 | tr -d '"' | awk -F " " '{print $1}'
}

declare -A bdfaddr
i=0
for bdf in $(linux_iter_pci 0108); do
	echo "  TransportID \"trtype:PCIe traddr:$bdf\" Nvme${i}"
	bdfaddr[$i]=$bdf
	let i=i+1
	if [ $i -gt 2 ]; then
		break
	fi
done

timing_enter multiconnection

timing_enter start_iscsi_tgt
iscsicleanup
# Start the iSCSI target without using stub
$rootdir/app/iscsi_tgt/iscsi_tgt -c $testdir/iscsi.conf -m 0x1 -p 0 -s 512 &
iscsipid=$!
echo "iSCSI target launched. pid: $iscsipid"
trap "killprocess $iscsipid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $iscsipid ${RPC_PORT}
timing_exit start_iscsi_tgt

$rpc_py -p 5261 add_portal_group 1 $TARGET_IP:$ISCSI_PORT
$rpc_py -p 5261 add_initiator_group 1 ALL $INITIATOR_IP/32

echo "Creating an iSCSI target node."
for i in `seq 0 1`; do
	$rpc_py -p 5261 construct_nvme_bdev -b "nvme${i}" -t "PCIe" -a "${bdfaddr[${i}]}"
	ls_guid=$($rpc_py -p 5261 construct_lvol_store "nvme${i}n1" "lvs${i}" -c 1048576)
	LUNs=""
	for j in `seq 0 63`; do
		lb_name=$($rpc_py construct_lvol_bdev -u $ls_guid lbd$j 10)
		lvol_bdevs+=("$lb_name")
		LUNs+="$lb_name:$j "
	done
	$rpc_py -p 5261 construct_target_node Target$i Target${i}_alias "$LUNs" "1:${i+1}" 256 1 0 0 0
done
sleep 1

echo "Logging in to iSCSI target."
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
sleep 1

echo "Running FIO"
$fio_py 131072 8 rw 5 verify
$fio_py 131072 8 write 5 verify
$fio_py 131072 8 randwrite 5 verify
$fio_py 131072 8 randrw 5 verify
sync

echo "INFO: Removing lvol bdevs"
for lvol_bdev in "${lvol_bdevs[@]}"; do
	$rpc_py delete_bdev $lvol_bdev
	echo -e "\tINFO: lvol bdev $lvol_bdev removed"
done
sleep 1

echo "INFO: Removing lvol stores"
$rpc_py destroy_lvol_store -l lvs0
$rpc_py destroy_lvol_store -l lvs1
echo -e "\tINFO: lvol store lvs0 lvs1 removed"

rm -f ./local-job0-0-verify.state
trap - SIGINT SIGTERM EXIT

rm -f ./local-job*
iscsicleanup
killprocess $iscsipid
timing_exit multiconnection
