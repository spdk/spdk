#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/scripts/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

function node_login_fio_logout()
{
	iscsiadm -m node --login -p $TARGET_IP:$PORT
	sleep 1
	$fio_py 512 1 write 2
	$fio_py 512 1 read 2
	iscsiadm -m node --logout -p $TARGET_IP:$PORT
	sleep 1
}

function iscsi_header_digest_test()
{
	# Enable HeaderDigest to CRC32C
	timing_enter HeaderDigest_enabled
	iscsiadm -m node -p $TARGET_IP:$PORT -o update -n node.conn[0].iscsi.HeaderDigest -v CRC32C
	node_login_fio_logout
	timing_exit HeaderDigest_enabled

	# Let iscsi target to decide its preference on
	# HeaderDigest based on its capability.
	timing_enter preferred
	iscsiadm -m node -p $TARGET_IP:$PORT -o update -n node.conn[0].iscsi.HeaderDigest -v CRC32C,None
	node_login_fio_logout
	timing_exit preferred
}

function iscsi_header_data_digest_test()
{
	# Only enable HeaderDigest to CRC32C
	timing_enter HeaderDigest_enabled
	iscsiadm -m node -p $TARGET_IP:$PORT -o update -n node.conn[0].iscsi.HeaderDigest -v CRC32C
	iscsiadm -m node -p $TARGET_IP:$PORT -o update -n node.conn[0].iscsi.DataDigest -v None
	node_login_fio_logout
	timing_exit HeaderDigest_enabled

	# Only enable DataDigest to CRC32C
	timing_enter DataDigest_enabled
	iscsiadm -m node -p $TARGET_IP:$PORT -o update -n node.conn[0].iscsi.HeaderDigest -v None
	iscsiadm -m node -p $TARGET_IP:$PORT -o update -n node.conn[0].iscsi.DataDigest -v CRC32C
	node_login_fio_logout
	timing_exit DataDigest_enabled

	# Let iscsi target to decide its preference on both
	# HeaderDigest and DataDigest based on its capability.
	timing_enter both_preferred
	iscsiadm -m node -p $TARGET_IP:$PORT -o update -n node.conn[0].iscsi.HeaderDigest -v CRC32C,None
	iscsiadm -m node -p $TARGET_IP:$PORT -o update -n node.conn[0].iscsi.DataDigest -v CRC32C,None
	node_login_fio_logout
	timing_exit both_preferred

	# Enable HeaderDigest and DataDigest both.
	timing_enter both_enabled
	iscsiadm -m node -p $TARGET_IP:$PORT -o update -n node.conn[0].iscsi.HeaderDigest -v CRC32C
	iscsiadm -m node -p $TARGET_IP:$PORT -o update -n node.conn[0].iscsi.DataDigest -v CRC32C
	node_login_fio_logout
	timing_exit both_enabled
}

timing_enter digests

# iSCSI target configuration
PORT=3260
INITIATOR_TAG=2
INITIATOR_NAME=ANY
NETMASK=$INITIATOR_IP/32
MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="python $rootdir/scripts/rpc.py"
fio_py="python $rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

$ISCSI_APP -c $testdir/iscsi.conf -m $ISCSI_TEST_CORE_MASK &
pid=$!
echo "Process pid: $pid"

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
# "1 0 0 0" ==> disable CHAP authentication
$rpc_py construct_target_node Target3 Target3_alias 'Malloc0:0' '1:2' 64 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$PORT

# iscsiadm installed by some Fedora releases loses DataDigest parameter.
# Check and avoid setting DataDigest.
DataDigestAbility=`iscsiadm -m node -p $TARGET_IP:$PORT | grep DataDigest || true`
if [ "$DataDigestAbility"x = x ]; then
	iscsi_header_digest_test
else
	iscsi_header_data_digest_test
fi

trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $pid
timing_exit digests
