#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

# $1 = "iso" - triggers isolation mode (setting up required environment).
# $2 = test type posix or vpp. defaults to posix.
iscsitestinit $1 $2

delete_tmp_files() {
	rm -f $testdir/iscsi2.json
	rm -f ./local-job0-0-verify.state
	rm -f ./local-job1-1-verify.state
}

if [ -z "$TARGET_IP" ]; then
	echo "TARGET_IP not defined in environment"
	exit 1
fi

if [ -z "$INITIATOR_IP" ]; then
	echo "INITIATOR_IP not defined in environment"
	exit 1
fi

timing_enter iscsi_fuzz_test

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=4096

rpc_py="$rootdir/scripts/rpc.py"

TEST_TIMEOUT=100

# This argument is used in addition to the test arguments in autotest_common.sh
for i in "$@"; do
        case "$i" in
                --timeout=*)
                        TEST_TIMEOUT="${i#*=}"
        esac
done

timing_enter start_iscsi_tgt

$ISCSI_APP -m 0xF &>$output_dir/iscsi_autofuzz_tgt_output.txt &
iscsipid=$!

trap 'killprocess $iscsipid; exit 1' SIGINT SIGTERM EXIT

waitforlisten $iscsipid
echo "iscsi_tgt is listening. Running tests..."
timing_exit start_iscsi_tgt

$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
$rpc_py iscsi_create_target_node disk1 disk1_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 256 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
waitforiscsidevices 1

trap 'iscsicleanup; killprocess $iscsipid; iscsitestfini $1 $2; delete_tmp_files; exit 1' SIGINT SIGTERM EXIT

$rootdir/test/app/fuzz/iscsi_fuzz/iscsi_fuzz -m 0xF0 -t $TEST_TIMEOUT 2>$output_dir/iscsi_autofuzz_logs.txt

$rpc_py save_config > $testdir/iscsi2.json

iscsicleanup
$rpc_py iscsi_delete_target_node 'iqn.2016-06.io.spdk:disk1'

# Delete malloc device
$rpc_py delete_malloc_bdev Malloc0


delete_tmp_files

trap - SIGINT SIGTERM EXIT

killprocess $iscsipid

iscsitestfini $1 $2

timing_exit iscsi_fuzz_test
