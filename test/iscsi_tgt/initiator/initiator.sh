#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

iscsitestinit

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

timing_enter start_iscsi_tgt

"${ISCSI_APP[@]}" -m 0x2 -p 1 -s 512 --wait-for-rpc &
pid=$!
echo "iSCSI target launched. pid: $pid"
trap 'killprocess $pid;exit 1' SIGINT SIGTERM EXIT
waitforlisten $pid
$rpc_py iscsi_set_options -o 30 -a 4
$rpc_py framework_start_init
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
# "Malloc0:0" ==> use Malloc0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "-d" ==> disable CHAP authentication
$rpc_py iscsi_create_target_node disk1 disk1_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 256 -d
sleep 1
trap 'killprocess $pid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

"$rootdir/test/bdev/bdevperf/bdevperf" --json <(initiator_json_config) -q 128 -o 4096 -w verify -t 5 -s 512
if [ $RUN_NIGHTLY -eq 1 ]; then
	"$rootdir/test/bdev/bdevperf/bdevperf" --json <(initiator_json_config) -q 128 -o 4096 -w unmap -t 5 -s 512
	"$rootdir/test/bdev/bdevperf/bdevperf" --json <(initiator_json_config) -q 128 -o 4096 -w flush -t 5 -s 512
	"$rootdir/test/bdev/bdevperf/bdevperf" --json <(initiator_json_config) -q 128 -o 4096 -w reset -t 10 -s 512
fi

trap - SIGINT SIGTERM EXIT

killprocess $pid

iscsitestfini
