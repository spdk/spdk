#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

ISCSI_PORT=3260

timing_enter gpt

cp $testdir/iscsi.conf.in $testdir/iscsi.conf

# create an aio image with 64MB
dd if=/dev/zero of=$testdir/aio.img bs=4096 count=16384

if [ ! -e $testdir/aio.img ]; then
	rm -rf $testdir/iscsi.conf
	timing_exit gpt
else
	echo "[AIO]" >> $testdir/iscsi.conf
	echo "AIO $testdir/aio.img AIO0" >> $testdir/iscsi.conf
	echo "[Rpc]" >> $testdir/iscsi.conf
	echo "  Enable Yes" >> $testdir/iscsi.conf
fi

timing_enter start_iscsi_tgt

# Start the iSCSI target
$ISCSI_APP -c $testdir/iscsi.conf -m 0x1 -p 0 -s 512 &
iscsipid=$!
echo "iSCSI target launched. pid: $iscsipid"
trap "killprocess $iscsipid; exit 1" SIGINT SIGTERM EXIT
waitforlisten $iscsipid 5260
echo "iSCSI target has started."

timing_exit start_iscsi_tgt

echo "Creating an iSCSI target node."
$rpc_py add_portal_group 1 $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group 1 ALL $INITIATOR_IP/32

# For file, we can directly part instead of calling nbd
parted -s $testdir/aio.img mklabel gpt mkpart first '0%' '50%' mkpart second '50%' '100%'
# change the GUID to SPDK GUID value
sgdisk -t 1:$SPDK_GPT_GUID $testdir/aio.img
sgdisk -t 2:$SPDK_GPT_GUID $testdir/aio.img

$rpc_py examine_bdev gpt AIO0
waitforbdev AIO0p1 $rootdir/scripts/rpc.py
waitforbdev AIO0p2 $rootdir/scripts/rpc.py
#construct two target_nodes and test it in the parallel way.
$rpc_py construct_target_node Target1 Target1_alias 'AIO0p1:0' '1:1' 64 1 0 0 0
$rpc_py construct_target_node Target2 Target2_alias 'AIO0p2:0' '1:1' 64 1 0 0 0
sleep 1

echo "Logging in to iSCSI target."
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
trap "iscsicleanup; killprocess $iscsipid; exit 1" SIGINT SIGTERM EXIT
sleep 1

echo "Running FIO"
$fio_py 4096 1 randrw 1 verify

rm -f ./local-job0-0-verify.state
trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $iscsipid
rm -f $testdir/iscsi.conf
rm -rf $testdir/aio.img

timing_exit gpt
