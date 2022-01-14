#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

iscsitestinit

BDEV_SIZE=64
BDEV_NEW_SIZE=128
BLOCK_SIZE=512
RESIZE_SOCK="/var/tmp/spdk-resize.sock"

rpc_py="$rootdir/scripts/rpc.py"

timing_enter start_iscsi_tgt

# Remove the sock file first
rm -f $RESIZE_SOCK

"${ISCSI_APP[@]}" -m 0x2 -p 1 -s 512 --wait-for-rpc &
pid=$!
echo "iSCSI target launched. pid: $pid"
trap 'killprocess $pid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT
waitforlisten $pid
$rpc_py framework_start_init
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py bdev_null_create Null0 $BDEV_SIZE $BLOCK_SIZE
# "Null0:0" ==> use Null0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "-d" ==> disable CHAP authentication
$rpc_py iscsi_create_target_node disk1 disk1_alias 'Null0:0' $PORTAL_TAG:$INITIATOR_TAG 256 -d
sleep 1
trap 'killprocess $pid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

# Start bdevperf with another sock file and iSCSI initiator
"$rootdir/test/bdev/bdevperf/bdevperf" -r $RESIZE_SOCK --json <(initiator_json_config) -q 16 -o 4096 -w read -t 5 -R -s 128 -z &
bdevperf_pid=$!
waitforlisten $bdevperf_pid $RESIZE_SOCK
# Resize the Bdev from iSCSI target
$rpc_py bdev_null_resize Null0 $BDEV_NEW_SIZE
# Obtain the Bdev from bdevperf with iSCSI initiator
num_block=$($rpc_py -s $RESIZE_SOCK bdev_get_bdevs | grep num_blocks | sed 's/[^[:digit:]]//g')
# Size is not changed as no IO sent yet and resize notification is deferred.
total_size=$((num_block * BLOCK_SIZE / 1048576))
if [ $total_size != $BDEV_SIZE ]; then
	echo "resize failed"
	exit 1
fi
sleep 2
# Start the bdevperf IO
$rootdir/test/bdev/bdevperf/bdevperf.py -s $RESIZE_SOCK perform_tests
# Obtain the Bdev from bdevperf with iSCSI initiator
num_block=$($rpc_py -s $RESIZE_SOCK bdev_get_bdevs | grep num_blocks | sed 's/[^[:digit:]]//g')
# Get the new bdev size in MiB.
total_size=$((num_block * BLOCK_SIZE / 1048576))
if [ $total_size != $BDEV_NEW_SIZE ]; then
	echo "resize failed"
	exit 1
fi

trap - SIGINT SIGTERM EXIT
killprocess $bdevperf_pid
killprocess $pid

iscsitestfini
