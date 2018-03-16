#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

if [ ! -d /usr/include/iscsi ]; then
	exit 0
fi

libiscsi_version=`grep LIBISCSI_API_VERSION /usr/include/iscsi/iscsi.h | head -1 | awk '{print $3}' | awk -F '(' '{print $2}' | awk -F ')' '{print $1}'`
if [ $libiscsi_version -lt 20150621 ]; then
	exit 0
fi


timing_enter initiator

# iSCSI target configuration
PORT=3260
INITIATOR_TAG=2
INITIATOR_NAME=ANY
NETMASK=$INITIATOR_IP/32
MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="python $rootdir/scripts/rpc.py"

timing_enter start_iscsi_tgt

$ISCSI_APP -c $testdir/iscsi.conf -m $ISCSI_TEST_CORE_MASK &
pid=$!

trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py add_portal_group 1 $TARGET_IP:$PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
# "Malloc0:0" ==> use Malloc0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "-d" ==> disable CHAP authentication
$rpc_py construct_target_node disk1 disk1_alias 'Malloc0:0' '1:2' 256 -d
sleep 1
trap "killprocess $pid; exit 1" SIGINT SIGTERM EXIT
$rootdir/test/lib/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -q 128 -s 4096 -w verify -t 5 -d 512
sleep 1

#$fio_py 4096 1 randrw 1 verify
#$fio_py 131072 32 randrw 1 verify


trap - SIGINT SIGTERM EXIT

killprocess $pid

report_test_completion "iscsi_initiator"
timing_exit initiator
