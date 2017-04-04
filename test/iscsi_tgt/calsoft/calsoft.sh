#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh

if [ ! -d /usr/local/calsoft ]; then
	echo "skipping calsoft tests"
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
timing_enter calsoft

# iSCSI target configuration
PORT=3260
RPC_PORT=5260
INITIATOR_TAG=2
INITIATOR_NAME=ALL
NETMASK=$INITIATOR_IP/32
MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="python $rootdir/scripts/rpc.py"
calsoft_py="python $testdir/calsoft.py"

# Copy the calsoft config file to /usr/local/etc
mkdir -p /usr/local/etc
cp $testdir/its.conf /usr/local/etc/
cp $testdir/auth.conf /usr/local/etc/

./app/iscsi_tgt/iscsi_tgt -c $testdir/iscsi.conf &
pid=$!
echo "Process pid: $pid"

trap "killprocess $pid; exit 1 " SIGINT SIGTERM EXIT

waitforlisten $pid ${RPC_PORT}
echo "iscsi_tgt is listening. Running tests..."

$rpc_py add_portal_group 1 $TARGET_IP:$PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
# "Malloc0:0" ==> use Malloc0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "0 0 0 1" ==> enable CHAP authentication using auth group 1
$rpc_py construct_target_node Target3 Target3_alias 'Malloc0:0' '1:2' 64 0 0 0 1
sleep 1

if [ "$1" ]; then
	$calsoft_py "$output_dir" "$1"
	failed=$?
else
	$calsoft_py "$output_dir"
	failed=$?
fi

trap - SIGINT SIGTERM EXIT

killprocess $pid
timing_exit calsoft
exit $failed
