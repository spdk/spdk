#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

# $1 = "iso" - triggers isolation mode (setting up required environment).
# $2 = test type posix or vpp. defaults to posix.
iscsitestinit $1 $2

function node_login_fio_logout() {
	for arg in "$@"; do
		iscsiadm -m node -p $TARGET_IP:$ISCSI_PORT -o update -n node.conn[0].iscsi.$arg
	done
	iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
	waitforiscsidevices 1
	$fio_py -p iscsi -i 512 -d 1 -t write -r 2
	$fio_py -p iscsi -i 512 -d 1 -t read -r 2
	iscsiadm -m node --logout -p $TARGET_IP:$ISCSI_PORT
	waitforiscsidevices 0
}

function iscsi_header_digest_test() {
	# Enable HeaderDigest to CRC32C
	timing_enter HeaderDigest_enabled
	node_login_fio_logout "HeaderDigest -v CRC32C"
	timing_exit HeaderDigest_enabled

	# Let iscsi target to decide its preference on
	# HeaderDigest based on its capability.
	timing_enter preferred
	node_login_fio_logout "HeaderDigest -v CRC32C,None"
	timing_exit preferred
}

function iscsi_header_data_digest_test() {
	# Only enable HeaderDigest to CRC32C
	timing_enter HeaderDigest_enabled
	node_login_fio_logout "HeaderDigest -v CRC32C" "DataDigest -v None"
	timing_exit HeaderDigest_enabled

	# Only enable DataDigest to CRC32C
	timing_enter DataDigest_enabled
	node_login_fio_logout "HeaderDigest -v None" "DataDigest -v CRC32C"
	timing_exit DataDigest_enabled

	# Let iscsi target to decide its preference on both
	# HeaderDigest and DataDigest based on its capability.
	timing_enter both_preferred
	node_login_fio_logout "HeaderDigest -v CRC32C,None" "DataDigest -v CRC32C,None"
	timing_exit both_preferred

	# Enable HeaderDigest and DataDigest both.
	timing_enter both_enabled
	node_login_fio_logout "HeaderDigest -v CRC32C" "DataDigest -v CRC32C"
	timing_exit both_enabled
}

timing_enter digests

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio.py"

timing_enter start_iscsi_tgt

$ISCSI_APP -m $ISCSI_TEST_CORE_MASK --wait-for-rpc &
pid=$!
echo "Process pid: $pid"

trap "killprocess $pid; iscsitestfini $1 $2; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py set_iscsi_options -o 30 -a 16
$rpc_py start_subsystem_init
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py add_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
# "Malloc0:0" ==> use Malloc0 blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "-d" ==> disable CHAP authentication
$rpc_py construct_target_node Target3 Target3_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 64 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT

# iscsiadm installed by some Fedora releases loses DataDigest parameter.
# Check and avoid setting DataDigest.
DataDigestAbility=$(iscsiadm -m node -p $TARGET_IP:$ISCSI_PORT | grep DataDigest || true)
if [ "$DataDigestAbility"x = x ]; then
	iscsi_header_digest_test
else
	iscsi_header_data_digest_test
fi

trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $pid
iscsitestfini $1 $2
timing_exit digests
