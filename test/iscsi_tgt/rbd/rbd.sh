#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

if ! hash ceph; then
	echo "Ceph not detected on this system; skipping RBD tests"
	exit 0
fi

timing_enter rbd_setup
rbd_setup $TARGET_IP $TARGET_NAMESPACE
trap "rbd_cleanup; exit 1" SIGINT SIGTERM EXIT
timing_exit rbd_setup

timing_enter rbd

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

$ISCSI_APP -c $testdir/iscsi.conf -m $ISCSI_TEST_CORE_MASK &
pid=$!

trap "killprocess $pid; rbd_cleanup; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py add_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py construct_rbd_bdev $RBD_POOL $RBD_NAME 4096
$rpc_py get_bdevs
# "Ceph0:0" ==> use Ceph0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "-d" ==> disable CHAP authentication
$rpc_py construct_target_node Target3 Target3_alias 'Ceph0:0' $PORTAL_TAG:$INITIATOR_TAG 64 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT

trap "iscsicleanup; killprocess $pid; rbd_cleanup; exit 1" SIGINT SIGTERM EXIT

sleep 1
$fio_py 4096 1 randrw 1 verify
$fio_py 131072 32 randrw 1 verify

rm -f ./local-job0-0-verify.state

trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $pid
rbd_cleanup

report_test_completion "iscsi_rbd"
timing_exit rbd
