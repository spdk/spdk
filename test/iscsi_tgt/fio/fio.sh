#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

delete_tmp_files() {
	rm -f $testdir/iscsi.conf
	rm -f ./local-job0-0-verify.state
}

function running_config() {
	# generate a config file from the running iscsi_tgt
	#  running_config.sh will leave the file at /tmp/iscsi.conf
	$testdir/running_config.sh $pid
	sleep 1

	# now start iscsi_tgt again using the generated config file
	# keep the same iscsiadm configuration to confirm that the
	#  config file matched the running configuration
	killprocess $pid
	trap "iscsicleanup; delete_tmp_files; exit 1" SIGINT SIGTERM EXIT

	timing_enter start_iscsi_tgt2

	$ISCSI_APP -c /tmp/iscsi.conf &
	pid=$!
	echo "Process pid: $pid"
	trap "iscsicleanup; killprocess $pid; delete_tmp_files; exit 1" SIGINT SIGTERM EXIT
	waitforlisten $pid
	echo "iscsi_tgt is listening. Running tests..."

	timing_exit start_iscsi_tgt2

	sleep 1
	$fio_py 4096 1 randrw 5
}

if [ -z "$TARGET_IP" ]; then
	echo "TARGET_IP not defined in environment"
	exit 1
fi

if [ -z "$INITIATOR_IP" ]; then
	echo "INITIATOR_IP not defined in environment"
	exit 1
fi

timing_enter fio

cp $testdir/iscsi.conf.in $testdir/iscsi.conf

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=4096

rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

$ISCSI_APP -c $testdir/iscsi.conf &
pid=$!
echo "Process pid: $pid"

trap "killprocess $pid; rm -f $testdir/iscsi.conf; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py add_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
# Create a RAID-0 bdev from two malloc bdevs
malloc_bdevs="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE) "
malloc_bdevs+="$($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
$rpc_py construct_raid_bdev -n raid0 -s 64 -r 0 -b "$malloc_bdevs"
# "raid0:0" ==> use raid0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "-d" ==> disable CHAP authentication
$rpc_py construct_target_node Target3 Target3_alias 'raid0:0' $PORTAL_TAG:$INITIATOR_TAG 64 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT

trap "iscsicleanup; killprocess $pid; delete_tmp_files; exit 1" SIGINT SIGTERM EXIT

sleep 1
$fio_py 4096 1 randrw 1 verify
$fio_py 131072 32 randrw 1 verify
$fio_py 524288 128 randrw 1 verify

if [ $RUN_NIGHTLY -eq 1 ]; then
	$fio_py 4096 1 write 300 verify

	# Run the running_config test which will generate a config file from the
	#  running iSCSI target, then kill and restart the iSCSI target using the
	#  generated config file
	# Temporarily disabled
	# running_config
fi

# Start hotplug test case.
$fio_py 1048576 128 rw 10 &
fio_pid=$!

sleep 3
set +e
# Delete raid0, Malloc0, Malloc1 blockdevs
$rpc_py destroy_raid_bdev 'raid0'
$rpc_py delete_malloc_bdev 'Malloc0'
$rpc_py delete_malloc_bdev 'Malloc1'

wait $fio_pid
fio_status=$?

if [ $fio_status -eq 0 ]; then
	echo "iscsi hotplug test: fio successful - expected failure"
	set -e
	exit 1
else
	echo "iscsi hotplug test: fio failed as expected"
fi

set -e

iscsicleanup
$rpc_py delete_target_node 'iqn.2016-06.io.spdk:Target3'

delete_tmp_files

trap - SIGINT SIGTERM EXIT

killprocess $pid
#echo 1 > /sys/bus/pci/rescan
#sleep 2
$rootdir/scripts/setup.sh

timing_exit fio
