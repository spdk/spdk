#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

if [ ! -d $CEPH_DIR ]; then
	echo "Ceph directory not detected on this system; skipping RBD tests"
	exit 0
fi

if [ -z "$TARGET_IP" ]; then
	echo "TARGET_IP not defined in environment"
	exit 1
fi

if [ -z "$INITIATOR_IP" ]; then
	echo "INITIATOR_IP not defined in environment"
	exit 1
fi

timing_enter rbd

# iSCSI target configuration
PORT=3260
RPC_PORT=5260
INITIATOR_TAG=2
INITIATOR_NAME=ALL
NETMASK=$INITIATOR_IP/32

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

$rootdir/app/iscsi_tgt/iscsi_tgt -c $testdir/iscsi.conf &
pid=$!

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid ${RPC_PORT}
echo "iscsi_tgt is listening. Running tests..."

$rpc_py add_portal_group 1 $TARGET_IP:$PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py construct_rbd_bdev $RBD_POOL $RBD_NAME 4096
# "Ceph0:0" ==> use Ceph0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "1 0 0 0" ==> disable CHAP authentication
$rpc_py construct_target_node Target3 Target3_alias 'Ceph0:0' '1:2' 64 1 0 0 0
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$PORT
iscsiadm -m node --login -p $TARGET_IP:$PORT

trap "iscsicleanup; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

sleep 1
$fio_py 4096 1 randrw 1 verify
$fio_py 131072 32 randrw 1 verify

rm -f ./local-job0-0-verify.state

trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $pid

timing_exit rbd
