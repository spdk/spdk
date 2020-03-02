#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

delete_tmp_conf_files() {
	rm -f /usr/local/etc/its.conf
}

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"
calsoft_py="$testdir/calsoft.py"

# Copy the calsoft config file to /usr/local/etc
mkdir -p /usr/local/etc
cp $testdir/its.conf /usr/local/etc/

# Append target ip to calsoft config
echo "IP=$TARGET_IP" >> /usr/local/etc/its.conf

timing_enter start_iscsi_tgt

"${ISCSI_APP[@]}" -m 0x1 --wait-for-rpc &
pid=$!
echo "Process pid: $pid"

trap 'killprocess $pid; delete_tmp_conf_files; exit 1 ' SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py load_subsystem_config < $testdir/iscsi.json
$rpc_py framework_start_init
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py iscsi_create_auth_group 1 -c 'user:root secret:tester'
$rpc_py iscsi_set_discovery_auth -g 1
$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py bdev_malloc_create -b MyBdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
# "MyBdev:0" ==> use MyBdev blockdev for LUN0
# "1:2" ==> map PortalGroup1 to InitiatorGroup2
# "64" ==> iSCSI queue depth 64
# "0 0 0 1" ==> enable CHAP authentication using auth group 1
$rpc_py iscsi_create_target_node Target3 Target3_alias 'MyBdev:0' $PORTAL_TAG:$INITIATOR_TAG 64 -g 1
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
delete_tmp_conf_files
exit $failed
