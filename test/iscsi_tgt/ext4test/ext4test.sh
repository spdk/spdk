#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

if [ ! -z $1 ]; then
	DPDK_DIR=$(readlink -f $1)
fi

if [ -z "$TARGET_IP" ]; then
	TARGET_IP=127.0.0.1
	echo "TARGET_IP not defined - using 127.0.0.1"
fi

if [ -z "$INITIATOR_IP" ]; then
	INITIATOR_IP=127.0.0.1
	echo "INITIATOR_IP not defined - using 127.0.0.1"
fi

if [ -z "$ISCSI_APP" ]; then
	ISCSI_APP=./app/iscsi_tgt/iscsi_tgt
fi

timing_enter ext4test

cp $testdir/iscsi.conf.in $testdir/iscsi.conf
$rootdir/scripts/gen_nvme.sh >> $testdir/iscsi.conf

# iSCSI target configuration
PORT=3260
RPC_PORT=5260
INITIATOR_TAG=2
INITIATOR_NAME=ALL
NETMASK=$INITIATOR_IP/32

rpc_py="python $rootdir/scripts/rpc.py"

$ISCSI_APP -c $testdir/iscsi.conf &
pid=$!
echo "Process pid: $pid"

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid ${RPC_PORT}
echo "iscsi_tgt is listening. Running tests..."

$rpc_py add_portal_group 1 $TARGET_IP:$PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "1 0 0 0" ==> disable CHAP authentication
if [ -z "$NO_NVME" ]; then
$rpc_py construct_target_node Target0 Target0_alias Nvme0n1:0 1:2 64 1 0 0 0
fi
$rpc_py construct_target_node Target1 Target1_alias Malloc0:0 1:2 64 1 0 0 0

sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$PORT
iscsiadm -m node --login -p $TARGET_IP:$PORT

trap 'for new_dir in `dir -d /mnt/*dir`; do umount $new_dir; rm -rf $new_dir; done; \
	iscsicleanup; killprocess $pid; exit 1' SIGINT SIGTERM EXIT

sleep 1


devs=$(iscsiadm -m session -P 3 | grep "Attached scsi disk" | awk '{print $4}')

for dev in $devs; do
	mkfs.ext4 -F /dev/$dev
	mkdir -p /mnt/${dev}dir
	mount -o sync /dev/$dev /mnt/${dev}dir

	rsync -qav --exclude=".git" $rootdir/ /mnt/${dev}dir/spdk

	make -C /mnt/${dev}dir/spdk DPDK_DIR=$DPDK_DIR clean
	make -C /mnt/${dev}dir/spdk DPDK_DIR=$DPDK_DIR -j16

	# Print out space consumed on target device to help decide
	#  if/when we need to increase the size of the malloc LUN
	df -h /dev/$dev

	rm -rf /mnt/${dev}dir/spdk
done

for dev in $devs; do
	umount /mnt/${dev}dir
	rm -rf /mnt/${dev}dir

	stats=( $(cat /sys/block/$dev/stat) )
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

rm -f $testdir/iscsi.conf
iscsicleanup
killprocess $pid
timing_exit ext4test
