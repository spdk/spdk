#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

function linux_iter_pci {
	lspci -mm -n -D | grep $1 | tr -d '"' | awk -F " " '{print $1}'
}

timing_enter filesystem

# iSCSI target configuration
PORT=3260
INITIATOR_TAG=2
INITIATOR_NAME=ALL
NETMASK=$INITIATOR_IP/32

kernel_dir=""

rpc_py="python $rootdir/scripts/rpc.py"

timing_enter start_iscsi_tgt

$ISCSI_APP -c $testdir/iscsi.conf -m $ISCSI_TEST_CORE_MASK &
pid=$!
echo "Process pid: $pid"

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

for bdf in $(linux_iter_pci 0108); do
	break
done

$rpc_py add_portal_group 1 $TARGET_IP:$PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py construct_nvme_bdev -b "Nvme0" -t "pcie" -a $bdf
ls_guid=$($rpc_py construct_lvol_store Nvme0n1 lvs_0)
$rpc_py construct_lvol_bdev -u $ls_guid  lbd_nest 16000
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "1 0 0 0" ==> disable CHAP authentication

$rpc_py construct_target_node Target1 Target1_alias 'lvs_0/lbd_nest:0' '1:2' 64 1 0 0 0
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$PORT
iscsiadm -m node --login -p $TARGET_IP:$PORT

trap "umount /mnt/device; rm -rf /mnt/device; iscsicleanup; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

sleep 1

mkdir -p  /mnt/device

dev=$(iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}')
echo $dev

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

	if [ $RUN_NIGHTLY -eq 1 ]; then
		cp -r $kernel_dir/linux-4.3.3 /mnt/device
		make -C /mnt/device/linux-4.3.3 clean
		make -C /mnt/device/linux-4.3.3 -j32
		rm -fr /mnt/device/linux-4.3.3
		umount /mnt/device
	else
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
	fi
done


$rpc_py delete_bdev lvs_0/lbd_nest
$rpc_py destroy_lvol_store -l lvs_0
rm -rf /mnt/device

trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $pid
timing_exit filesystem
