#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

iscsitestinit

delete_tmp_files() {
	rm -f $testdir/iscsi2.json
	rm -f ./local-job0-0-verify.state
	rm -f ./local-job1-1-verify.state
}

function running_config() {
	# dump a config file from the running iscsi_tgt
	$rpc_py save_config > $testdir/iscsi2.json
	sleep 1

	# now start iscsi_tgt again using the generated config file
	# keep the same iscsiadm configuration to confirm that the
	#  config file matched the running configuration
	killprocess $pid
	trap 'iscsicleanup; delete_tmp_files; exit 1' SIGINT SIGTERM EXIT

	timing_enter start_iscsi_tgt2

	"${ISCSI_APP[@]}" --wait-for-rpc &
	pid=$!
	echo "Process pid: $pid"
	trap 'iscsicleanup; killprocess $pid; delete_tmp_files; exit 1' SIGINT SIGTERM EXIT
	waitforlisten $pid

	$rpc_py load_config < $testdir/iscsi2.json

	echo "iscsi_tgt is listening. Running tests..."

	timing_exit start_iscsi_tgt2

	sleep 1
	$fio_py -p iscsi -i 4096 -d 1 -t randrw -r 5
}

if [ -z "$TARGET_IP" ]; then
	echo "TARGET_IP not defined in environment"
	exit 1
fi

if [ -z "$INITIATOR_IP" ]; then
	echo "INITIATOR_IP not defined in environment"
	exit 1
fi

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=4096

rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

"${ISCSI_APP[@]}" --wait-for-rpc &
pid=$!
echo "Process pid: $pid"

trap 'killprocess $pid; exit 1' SIGINT SIGTERM EXIT

waitforlisten $pid

$rpc_py load_config < $testdir/iscsi.json

echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
# Create a RAID-0 bdev from two malloc bdevs
malloc_bdevs="$($rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE) "
malloc_bdevs+="$($rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
$rpc_py bdev_raid_create -n raid0 -z 64 -r 0 -b "$malloc_bdevs"
bdev=$($rpc_py bdev_malloc_create 1024 512)
# "raid0:0" ==> use raid0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "-d" ==> disable CHAP authentication
$rpc_py iscsi_create_target_node Target3 Target3_alias "raid0:0 ${bdev}:1" $PORTAL_TAG:$INITIATOR_TAG 64 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
waitforiscsidevices 2

trap 'iscsicleanup; killprocess $pid; iscsitestfini; delete_tmp_files; exit 1' SIGINT SIGTERM EXIT

$fio_py -p iscsi -i 4096 -d 1 -t randrw -r 1 -v
$fio_py -p iscsi -i 131072 -d 32 -t randrw -r 1 -v
$fio_py -p iscsi -i 524288 -d 128 -t randrw -r 1 -v
$fio_py -p iscsi -i 1048576 -d 1024 -t read -r 1 -n 4

if [ $RUN_NIGHTLY -eq 1 ]; then
	$fio_py -p iscsi -i 4096 -d 1 -t write -r 300 -v

	# Run the running_config test which will generate a config file from the
	#  running iSCSI target, then kill and restart the iSCSI target using the
	#  generated config file
	# Temporarily disabled
	# running_config
fi

# Start hotplug test case.
$fio_py -p iscsi -i 1048576 -d 128 -t rw -r 10 &
fio_pid=$!

sleep 3

# Delete raid0 blockdev
$rpc_py bdev_raid_delete 'raid0'

# Delete all allocated malloc blockdevs
for malloc_bdev in $malloc_bdevs; do
	$rpc_py bdev_malloc_delete $malloc_bdev
done

# Delete malloc device
$rpc_py bdev_malloc_delete ${bdev}

fio_status=0
wait $fio_pid || fio_status=$?

if [ $fio_status -eq 0 ]; then
	echo "iscsi hotplug test: fio successful - expected failure"
	exit 1
else
	echo "iscsi hotplug test: fio failed as expected"
fi

iscsicleanup
$rpc_py iscsi_delete_target_node 'iqn.2016-06.io.spdk:Target3'

delete_tmp_files

trap - SIGINT SIGTERM EXIT

killprocess $pid

iscsitestfini
