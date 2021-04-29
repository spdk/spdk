#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

iscsitestinit

rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio-wrapper"

CONNECTION_NUMBER=30

# Remove lvol bdevs and stores.
function remove_backends() {
	echo "INFO: Removing lvol bdevs"
	for i in $(seq 1 $CONNECTION_NUMBER); do
		lun="lvs0/lbd_$i"
		$rpc_py bdev_lvol_delete $lun
		echo -e "\tINFO: lvol bdev $lun removed"
	done
	sleep 1

	echo "INFO: Removing lvol stores"
	$rpc_py bdev_lvol_delete_lvstore -l lvs0
	echo "INFO: lvol store lvs0 removed"

	echo "INFO: Removing NVMe"
	$rpc_py bdev_nvme_detach_controller Nvme0

	return 0
}

timing_enter start_iscsi_tgt
"${ISCSI_APP[@]}" --wait-for-rpc &
iscsipid=$!
echo "iSCSI target launched. pid: $iscsipid"
trap 'remove_backends; iscsicleanup; killprocess $iscsipid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

waitforlisten $iscsipid
$rpc_py iscsi_set_options -o 30 -a 128
$rpc_py framework_start_init
$rootdir/scripts/gen_nvme.sh | $rpc_py load_subsystem_config
timing_exit start_iscsi_tgt

$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK

echo "Creating an iSCSI target node."
ls_guid=$($rpc_py bdev_lvol_create_lvstore "Nvme0n1" "lvs0" -c 1048576)

# Assign even size for each lvol_bdev.
get_lvs_free_mb $ls_guid
lvol_bdev_size=$((free_mb / CONNECTION_NUMBER))
for i in $(seq 1 $CONNECTION_NUMBER); do
	$rpc_py bdev_lvol_create -u $ls_guid lbd_$i $lvol_bdev_size
done

for i in $(seq 1 $CONNECTION_NUMBER); do
	lun="lvs0/lbd_$i:0"
	$rpc_py iscsi_create_target_node Target$i Target${i}_alias "$lun" $PORTAL_TAG:$INITIATOR_TAG 256 -d
done
sleep 1

echo "Logging into iSCSI target."
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
waitforiscsidevices $CONNECTION_NUMBER

echo "Running FIO"
$fio_py -p iscsi -i 131072 -d 64 -t randrw -r 5
$fio_py -p iscsi -i 262144 -d 16 -t randwrite -r 10
sync

trap - SIGINT SIGTERM EXIT

rm -f ./local-job*
iscsicleanup
remove_backends
killprocess $iscsipid
iscsitestfini
