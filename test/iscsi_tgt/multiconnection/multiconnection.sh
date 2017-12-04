#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/iscsi_fio.py"

CONNECTION_NUMBER=30

# Remove lvol bdevs and stores.
function remove_backends()
{
	echo "INFO: Removing lvol bdevs"
	for i in `seq 1 $CONNECTION_NUMBER`; do
		lun="lvs0/lbd_$i"
		$rpc_py delete_bdev $lun
		echo -e "\tINFO: lvol bdev $lun removed"
	done
	sleep 1

	echo "INFO: Removing lvol stores"
	$rpc_py destroy_lvol_store -l lvs0
	echo "INFO: lvol store lvs0 removed"

	return 0
}

set -e
timing_enter multiconnection

# Create conf file for iscsi multiconnection.
cat > $testdir/iscsi.conf << EOL
[iSCSI]
  NodeBase "iqn.2016-06.io.spdk"
  AuthFile /usr/local/etc/spdk/auth.conf
  Timeout 30
  DiscoveryAuthMethod Auto
  MaxSessions 128
  ImmediateData Yes
  ErrorRecoveryLevel 0
EOL

# Get nvme info through filtering gen_nvme.sh's result.
$rootdir/scripts/gen_nvme.sh >> $testdir/iscsi.conf

timing_enter start_iscsi_tgt
# Start the iSCSI target without using stub.
$rootdir/app/iscsi_tgt/iscsi_tgt -c $testdir/iscsi.conf &
iscsipid=$!
echo "iSCSI target launched. pid: $iscsipid"
trap "remove_backends; iscsicleanup; killprocess $iscsipid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $iscsipid
timing_exit start_iscsi_tgt

$rpc_py add_portal_group 1 $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group 1 ANY $INITIATOR_IP/32

echo "Creating an iSCSI target node."
ls_guid=$($rpc_py construct_lvol_store "Nvme0n1" "lvs0" -c 1048576)

# Assign even size for each lvol_bdev.
get_lvs_free_mb $ls_guid
lvol_bdev_size=$(($free_mb/$CONNECTION_NUMBER))
for i in `seq 1 $CONNECTION_NUMBER`; do
	$rpc_py construct_lvol_bdev -u $ls_guid lbd_$i $lvol_bdev_size
done

for i in `seq 1 $CONNECTION_NUMBER`; do
	lun="lvs0/lbd_$i:0"
	$rpc_py construct_target_node Target$i Target${i}_alias "$lun" "1:1" 256 -d
done
sleep 1

echo "Logging into iSCSI target."
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
sleep 1

echo "Running FIO"
dev_list=$(get_devices_list iscsi)
$fio_py $dev_list 131072 64 randrw 5
$fio_py $dev_list 262144 16 randwrite 10
sync

trap - SIGINT SIGTERM EXIT
remove_backends

rm -f $testdir/iscsi.conf
rm -f ./local-job*
iscsicleanup
killprocess $iscsipid
timing_exit multiconnection
