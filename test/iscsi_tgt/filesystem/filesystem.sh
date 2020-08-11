#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh
source $rootdir/scripts/common.sh

iscsitestinit

rpc_py="$rootdir/scripts/rpc.py"
# Remove lvol bdevs and stores.
function remove_backends() {
	echo "INFO: Removing lvol bdev"
	$rpc_py bdev_lvol_delete "lvs_0/lbd_0"

	echo "INFO: Removing lvol stores"
	$rpc_py bdev_lvol_delete_lvstore -l lvs_0

	echo "INFO: Removing NVMe"
	$rpc_py bdev_nvme_detach_controller Nvme0

	return 0
}

timing_enter start_iscsi_tgt

"${ISCSI_APP[@]}" -m $ISCSI_TEST_CORE_MASK --wait-for-rpc &
pid=$!
echo "Process pid: $pid"

trap 'killprocess $pid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py iscsi_set_options -o 30 -a 16
$rpc_py framework_start_init
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

bdf=$(get_first_nvme_bdf)
$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py bdev_nvme_attach_controller -b "Nvme0" -t "pcie" -a $bdf

ls_guid=$($rpc_py bdev_lvol_create_lvstore Nvme0n1 lvs_0)
free_mb=$(get_lvs_free_mb "$ls_guid")
# Using maximum 2048MiB to reduce the test time
if [ $free_mb -gt 2048 ]; then
	$rpc_py bdev_lvol_create -u $ls_guid lbd_0 2048
else
	$rpc_py bdev_lvol_create -u $ls_guid lbd_0 $free_mb
fi
# "lvs_0/lbd_0:0" ==> use lvs_0/lbd_0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "256" ==> iSCSI queue depth 256
# "-d" ==> disable CHAP authentication
$rpc_py iscsi_create_target_node Target1 Target1_alias 'lvs_0/lbd_0:0' $PORTAL_TAG:$INITIATOR_TAG 256 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
waitforiscsidevices 1

trap 'iscsicleanup; remove_backends; umount /mnt/device; rm -rf /mnt/device; killprocess $pid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

mkdir -p /mnt/device

dev=$(iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}')

waitforfile /dev/$dev
parted -s /dev/$dev mklabel msdos
parted -s /dev/$dev mkpart primary '0%' '100%'
sleep 1

function filesystem_test() {
	fstype=$1

	make_filesystem ${fstype} /dev/${dev}1
	mount /dev/${dev}1 /mnt/device
	if [ $RUN_NIGHTLY -eq 1 ]; then
		fio -filename=/mnt/device/test -direct=1 -iodepth 64 -thread=1 -invalidate=1 -rw=randwrite -ioengine=libaio -bs=4k \
			-size=1024M -name=job0
		umount /mnt/device

		iscsiadm -m node --logout
		waitforiscsidevices 0
		iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
		waitforiscsidevices 1

		dev=$(iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}')

		waitforfile /dev/${dev}1
		mount -o rw /dev/${dev}1 /mnt/device
		if [ -f "/mnt/device/test" ]; then
			echo "File existed."
			fio -filename=/mnt/device/test -direct=1 -iodepth 64 -thread=1 -invalidate=1 -rw=randread \
				-ioengine=libaio -bs=4k -runtime=20 -time_based=1 -name=job0
		else
			echo "File doesn't exist."
			exit 1
		fi

		rm -rf /mnt/device/test
		umount /mnt/device
	else
		touch /mnt/device/aaa
		umount /mnt/device

		iscsiadm -m node --logout
		waitforiscsidevices 0
		iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
		waitforiscsidevices 1

		dev=$(iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}')

		waitforfile /dev/${dev}1
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
}

run_test "iscsi_tgt_filesystem_ext4" filesystem_test "ext4"
run_test "iscsi_tgt_filesystem_btrfs" filesystem_test "btrfs"
run_test "iscsi_tgt_filesystem_xfs" filesystem_test "xfs"

rm -rf /mnt/device

trap - SIGINT SIGTERM EXIT

iscsicleanup
remove_backends
killprocess $pid
iscsitestfini
