#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

iscsitestinit

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
	node_login_fio_logout "HeaderDigest -v CRC32C"

	# Let iscsi target to decide its preference on
	# HeaderDigest based on its capability.
	node_login_fio_logout "HeaderDigest -v CRC32C,None"
}

function iscsi_header_data_digest_test() {
	# Only enable HeaderDigest to CRC32C
	node_login_fio_logout "HeaderDigest -v CRC32C" "DataDigest -v None"

	# Only enable DataDigest to CRC32C
	node_login_fio_logout "HeaderDigest -v None" "DataDigest -v CRC32C"

	# Let iscsi target to decide its preference on both
	# HeaderDigest and DataDigest based on its capability.
	node_login_fio_logout "HeaderDigest -v CRC32C,None" "DataDigest -v CRC32C,None"

	# Enable HeaderDigest and DataDigest both.
	node_login_fio_logout "HeaderDigest -v CRC32C" "DataDigest -v CRC32C"
}

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"
fio_py="$rootdir/scripts/fio-wrapper"

timing_enter start_iscsi_tgt

"${ISCSI_APP[@]}" -m $ISCSI_TEST_CORE_MASK --wait-for-rpc &
pid=$!
echo "Process pid: $pid"

trap 'killprocess $pid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py iscsi_set_options -o 30 -a 16
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
$rpc_py iscsi_create_target_node Target3 Target3_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 64 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT

# iscsiadm installed by some Fedora releases loses the ability to set DataDigest parameter.
# Check and avoid setting DataDigest.
DataDigestAbility=$(iscsiadm -m node -p $TARGET_IP:$ISCSI_PORT -o update -n node.conn[0].iscsi.DataDigest -v None 2>&1 || true)
if [ "$DataDigestAbility"x != x ]; then
	run_test "iscsi_tgt_digest" iscsi_header_digest_test
else
	run_test "iscsi_tgt_data_digest" iscsi_header_data_digest_test
fi

trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $pid
iscsitestfini
