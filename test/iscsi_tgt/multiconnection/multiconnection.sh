#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

TARGET_IP=127.0.0.1
INITIATOR_IP=127.0.0.1
ISCSI_PORT=3260

LVOL_BDEV_SIZE=10
CONNECTION_NUMBER=30

# Remove lvol bdevs and stores
function remove_backends()
{
	echo "INFO: Removing lvol bdevs"
	for i in `seq 1 $CONNECTION_NUMBER`; do
		lun="lvs$i/lbd_$i"
		$rpc_py delete_bdev $lun
		echo -e "\tINFO: lvol bdev $lun removed"
	done
	sleep 1

	echo "INFO: Removing lvol stores"
	for i in `seq 1 $CONNECTION_NUMBER`; do
		$rpc_py destroy_lvol_store -l lvs$i
		echo -e "\tINFO: lvol store lvs$i removed"
	done
	return 0
}

set -e

# Create conf file for iscsi multiconnection
cat > $testdir/iscsi.conf << EOL
[iSCSI]
  NodeBase "iqn.2016-06.io.spdk"
  AuthFile /usr/local/etc/spdk/auth.conf
  Timeout 30
  DiscoveryAuthMethod Auto
  MaxSessions 160
  ImmediateData Yes
  ErrorRecoveryLevel 0
[Rpc]
  Enable Yes
[Split]
  Split Nvme0n1 40 128
EOL

# Get nvme info through filtering gen_nvme.sh's result.
$rootdir/scripts/gen_nvme.sh >> $testdir/iscsi.conf
bdfaddr=$($rootdir/scripts/gen_nvme.sh | grep -e '0000:[0-9a-f]\{2\}:[0-9a-f]\{2\}\.[0-9]' -o | head -1)
sed -i '15,$d' $testdir/iscsi.conf

timing_enter multiconnection
timing_enter start_iscsi_tgt
iscsicleanup

# Start the iSCSI target without using stub
$rootdir/app/iscsi_tgt/iscsi_tgt -c $testdir/iscsi.conf &
iscsipid=$!
echo "iSCSI target launched. pid: $iscsipid"
trap "remove_backends; iscsicleanup; killprocess $iscsipid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $iscsipid ${RPC_PORT}
timing_exit start_iscsi_tgt

$rpc_py add_portal_group 1 $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group 1 ANY $INITIATOR_IP/32

echo "Creating an iSCSI target node."

for i in `seq 1 $CONNECTION_NUMBER`; do
	ls_guid=$($rpc_py construct_lvol_store "Nvme0n1p$i" "lvs$i" -c 1048576)
	lb_name=$($rpc_py construct_lvol_bdev -u $ls_guid lbd_$i $LVOL_BDEV_SIZE)
done
for i in `seq 1 $CONNECTION_NUMBER`; do
	lun="lvs$i/lbd_$i:0"
	$rpc_py construct_target_node Target$i Target${i}_alias "$lun" "1:1" 256 1 0 0 0
done
sleep 1

echo "Logging in to iSCSI target."
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
sleep 1

echo "Running FIO"
$fio_py 131072 64 randrw 5
$fio_py 262144 16 randwrite 10
sync

trap - SIGINT SIGTERM EXIT
remove_backends

rm -f $testdir/iscsi.conf
rm -f ./local-job*
iscsicleanup
kill $iscsipid
timing_exit multiconnection
