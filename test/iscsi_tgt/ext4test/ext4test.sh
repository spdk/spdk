#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

# $1 = "iso" - triggers isolation mode (setting up required environment).
# $2 = test type posix or vpp. defaults to posix.
iscsitestinit $1 $2

rpc_py="$rootdir/scripts/rpc.py"
node_base="iqn.2013-06.com.intel.ch.spdk"

timing_enter start_iscsi_tgt

$ISCSI_APP --wait-for-rpc &
pid=$!
echo "Process pid: $pid"

trap '$rpc_py bdev_split_delete Name0n1 || true; killprocess $pid; iscsitestfini $1 $2; exit 1' SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py iscsi_set_options -o 30 -a 4 -b $node_base
$rpc_py framework_start_init
$rootdir/scripts/gen_nvme.sh --json | $rpc_py load_subsystem_config
$rpc_py bdev_malloc_create 512 4096 --name Malloc0
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py bdev_error_create 'Malloc0'
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "-d" ==> disable CHAP authentication
$rpc_py iscsi_create_target_node Target0 Target0_alias EE_Malloc0:0 1:2 64 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
waitforiscsidevices 1

trap 'for new_dir in $(dir -d /mnt/*dir); do umount $new_dir; rm -rf $new_dir; done;
	iscsicleanup; killprocess $pid; iscsitestfini $1 $2; exit 1' SIGINT SIGTERM EXIT

echo "Test error injection"
$rpc_py bdev_error_inject_error EE_Malloc0 'all' 'failure' -n 1000

dev=$(iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}')

set +e
waitforfile /dev/$dev
if mkfs.ext4 -F /dev/$dev; then
	echo "mkfs successful - expected failure"
	iscsicleanup
	killprocess $pid
	exit 1
else
	echo "mkfs failed as expected"
fi
set -e

iscsicleanup
$rpc_py bdev_error_inject_error EE_Malloc0 'clear' 'failure'
$rpc_py iscsi_delete_target_node $node_base:Target0
echo "Error injection test done"

if [ -z "$NO_NVME" ]; then
	bdev_size=$(get_bdev_size Nvme0n1)
	split_size=$((bdev_size/2))
	if [ $split_size -gt 10000 ]; then
		split_size=10000
	fi
	$rpc_py bdev_split_create Nvme0n1 2 -s $split_size
	$rpc_py iscsi_create_target_node Target1 Target1_alias Nvme0n1p0:0 1:2 64 -d
fi

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
waitforiscsidevices 1

devs=$(iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}')

for dev in $devs; do
	mkfs.ext4 -F /dev/$dev
	mkdir -p /mnt/${dev}dir
	mount -o sync /dev/$dev /mnt/${dev}dir

	rsync -qav --exclude=".git" --exclude="*.o" $rootdir/ /mnt/${dev}dir/spdk

	make -C /mnt/${dev}dir/spdk clean
	(cd /mnt/${dev}dir/spdk && ./configure $config_params)
	make -C /mnt/${dev}dir/spdk -j16

	# Print out space consumed on target device to help decide
	#  if/when we need to increase the size of the malloc LUN
	df -h /dev/$dev

	rm -rf /mnt/${dev}dir/spdk
done

for dev in $devs; do
	umount /mnt/${dev}dir
	rm -rf /mnt/${dev}dir

	stats=($(cat /sys/block/$dev/stat))
	echo ""
	echo "$dev stats"
	printf "READ  IO cnt: % 8u merges: % 8u sectors: % 8u ticks: % 8u\n" \
		${stats[0]} ${stats[1]} ${stats[2]} ${stats[3]}
	printf "WRITE IO cnt: % 8u merges: % 8u sectors: % 8u ticks: % 8u\n" \
		${stats[4]} ${stats[5]} ${stats[6]} ${stats[7]}
	printf "in flight: % 8u io ticks: % 8u time in queue: % 8u\n" \
		${stats[8]} ${stats[9]} ${stats[10]}
	echo ""
done

trap - SIGINT SIGTERM EXIT

iscsicleanup
$rpc_py bdev_split_delete Nvme0n1
$rpc_py bdev_error_delete EE_Malloc0

if [ -z "$NO_NVME" ]; then
	$rpc_py bdev_nvme_detach_controller Nvme0
fi

killprocess $pid
iscsitestfini $1 $2
report_test_completion "nightly_iscsi_ext4test"
