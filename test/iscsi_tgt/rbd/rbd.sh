#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

iscsitestinit

timing_enter rbd_setup
rbd_setup $TARGET_IP $TARGET_NAMESPACE
trap 'rbd_cleanup; exit 1' SIGINT SIGTERM EXIT
timing_exit rbd_setup

rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

"${ISCSI_APP[@]}" -m $ISCSI_TEST_CORE_MASK --wait-for-rpc &
pid=$!

trap 'killprocess $pid; rbd_cleanup; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py iscsi_set_options -o 30 -a 16
$rpc_py framework_start_init
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
rbd_bdev="$($rpc_py bdev_rbd_create $RBD_POOL $RBD_NAME 4096)"
$rpc_py bdev_get_bdevs

$rpc_py bdev_rbd_resize $rbd_bdev 2000
num_block=$($rpc_py bdev_get_bdevs | grep num_blocks | sed 's/[^[:digit:]]//g')
# get the bdev size in MiB.
total_size=$((num_block * 4096 / 1048576))
if [ $total_size != 2000 ]; then
	echo "resize failed."
	exit 1
fi
# "Ceph0:0" ==> use Ceph0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "-d" ==> disable CHAP authentication
$rpc_py iscsi_create_target_node Target3 Target3_alias 'Ceph0:0' $PORTAL_TAG:$INITIATOR_TAG 64 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
waitforiscsidevices 1

trap 'iscsicleanup; killprocess $pid; rbd_cleanup; exit 1' SIGINT SIGTERM EXIT

$fio_py -p iscsi -i 4096 -d 1 -t randrw -r 1 -v
$fio_py -p iscsi -i 131072 -d 32 -t randrw -r 1 -v

rm -f ./local-job0-0-verify.state

trap - SIGINT SIGTERM EXIT

iscsicleanup
$rpc_py bdev_rbd_delete $rbd_bdev
killprocess $pid
rbd_cleanup

iscsitestfini
