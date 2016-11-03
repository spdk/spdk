#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/scripts/autotest_common.sh

if [ -z "$TARGET_IP" ]; then
	echo "TARGET_IP not defined in environment"
	exit 1
fi

if [ -z "$INITIATOR_IP" ]; then
	echo "INITIATOR_IP not defined in environment"
	exit 1
fi

timing_enter filesystem

# iSCSI target configuration
PORT=3260
RPC_PORT=5260
INITIATOR_TAG=2
INITIATOR_NAME=ALL
NETMASK=$INITIATOR_IP/32
MALLOC_BDEV_SIZE=256
MALLOC_BLOCK_SIZE=512

rpc_py="python $rootdir/scripts/rpc.py"

./app/iscsi_tgt/iscsi_tgt -c $testdir/iscsi.conf &
pid=$!
echo "Process pid: $pid"

trap "process_core; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid ${RPC_PORT}
echo "iscsi_tgt is listening. Running tests..."

$rpc_py add_portal_group 1 $TARGET_IP:$PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
# "Malloc0:0" ==> use Malloc0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "1 0 0 0" ==> disable CHAP authentication
$rpc_py construct_target_node Target3 Target3_alias 'Malloc0:0' '1:2' 256 1 0 0 0
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$PORT
iscsiadm -m node --login -p $TARGET_IP:$PORT

trap "umount /mnt/device; rm -rf /mnt/device; iscsicleanup; process_core; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

sleep 1

mkdir -p  /mnt/device

dev=$(iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}')

parted -s /dev/$dev mklabel msdos
parted -s /dev/$dev mkpart primary '0%' '100%'
sleep 1

for fstype in "ext4" "btrfs" "xfs"; do

	if [ "$fstype" == "ext4" ]; then
		mkfs.${fstype} -F /dev/${dev}1
	else
		mkfs.${fstype} -f /dev/${dev}1
	fi
	mount /dev/${dev}1 /mnt/device
	touch /mnt/device/aaa
	umount /mnt/device

	iscsiadm -m node --logout
	sleep 1
	iscsiadm -m node --login -p $TARGET_IP:$PORT
	sleep 1
	dev=$(iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}')
	mount -o rw /dev/${dev}1 /mnt/device

	if [ -f "/mnt/device/aaa" ]; then
		echo "File existed."
	else
		echo "File doesn't exist."
		exit 1
	fi

	rm -rf /mnt/device/aaa
	umount /mnt/device
done

rm -rf /mnt/device

trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $pid
timing_exit filesystem
