#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

timing_enter initiator

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

rpc_py="python $rootdir/scripts/rpc.py"

timing_enter start_iscsi_tgt

# Start the iSCSI target without using stub
# Reason: Two SPDK processes will be started
$ISCSI_APP -m 0x2 -p 1 -s 512 --wait-for-rpc &
pid=$!
echo "iSCSI target launched. pid: $pid"
trap "killprocess $pid;exit 1" SIGINT SIGTERM EXIT
waitforlisten $pid
$rpc_py set_iscsi_options -o 30 -a 4 -s
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
$rpc_py construct_target_node disk1 disk1_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 256 -d
sleep 1
trap "killprocess $pid; rm -f $testdir/bdev.conf; exit 1" SIGINT SIGTERM EXIT

# Prepare config file for iSCSI initiator
cp $testdir/bdev.conf.in $testdir/bdev.conf
echo "[iSCSI_Initiator]" >> $testdir/bdev.conf
echo "  URL iscsi://$TARGET_IP/iqn.2016-06.io.spdk:disk1/0 iSCSI0" >> $testdir/bdev.conf
$rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -q 128 -o 4096 -w verify -t 5 -s 512
if [ $RUN_NIGHTLY -eq 1 ]; then
    $rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -q 128 -o 4096 -w unmap -t 5 -s 512
    $rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -q 128 -o 4096 -w flush -t 5 -s 512
    $rootdir/test/bdev/bdevperf/bdevperf -c $testdir/bdev.conf -q 128 -o 4096 -w reset -t 10 -s 512
fi
rm -f $testdir/bdev.conf

$rpc_py delete_target_node 'iqn.2016-06.io.spdk:disk1'

$rpc_py construct_target_node disk1 disk1_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 256

$rpc_py add_iscsi_auth_group 1 'user:host1 secret:hsecret1'
$rpc_py add_iscsi_auth_group 2 'user:host1 secret:hsecret1 muser:target1 msecret:tsecret1'

# This section expects success of CHAP authentication for discovery session.
timing_enter discovery_auth_success

$rpc_py set_iscsi_discovery_auth -d

# Discovery requires no CHAP. Initiator does not use CHAP.
iscsi-ls iscsi://$TARGET_IP:$ISCSI_PORT

$rpc_py set_iscsi_discovery_auth -g 1

# Discovery uses CHAP if initiator uses. Initiator does not use CHAP.
iscsi-ls iscsi://$TARGET_IP:$ISCSI_PORT

# Discovery uses CHAP if initiator uses. Initiaotor uses one-way CHAP.
iscsi-ls iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT

$rpc_py set_iscsi_discovery_auth -g 2

# Discovery uses CHAP if initiator uses. Initiator uses mutual CHAP.
iscsi-ls iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/?target_user=target1\&target_password=tsecret1

$rpc_py set_iscsi_discovery_auth -g 1 -r

# Discovery requires (one-way) CHAP. Initiator uses one-way CHAP.
iscsi-ls iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT

$rpc_py set_iscsi_discovery_auth -g 2 -r -m

# Discovery requires mutual CHAP. Initiator uses mutual CHAP.
iscsi-ls iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/?target_user=target1\&target_password=tsecret1

timing_exit discovery_auth_success

# This section expects failure of CHAP authentication for discovery session.
timing_enter discovery_auth_failure
set +e

$rpc_py set_iscsi_discovery_auth -d

# Discovery requires no CHAP. Initiaotor uses one-way CHAP.
iscsi-ls iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT
if [ $? -eq 0 ]; then
	echo "CHAP authentication succeed - expected failure"
	iscsicleanup
	killprocess $pid
	exit 1
else
	echo "CHAP authentication failed as expected"
fi

$rpc_py set_iscsi_discovery_auth -g 1 -r

# Discovery requires (one-way) CHAP. Initiator does not use CHAP.
iscsi-ls iscsi://$TARGET_IP:$ISCSI_PORT
if [ $? -eq 0 ]; then
	echo "CHAP authentication succeed - expected failure"
	iscsicleanup
	killprocess $pid
	exit 1
else
	echo "CHAP authentication failed as expected"
fi

# Discovery requires (one-way) CHAP. Initiator uses mutual CHAP.
iscsi-ls iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/?target_user=target1\&target_password=tsecret1
if [ $? -eq 0 ]; then
	echo "CHAP authentication succeed - expected failure"
	iscsicleanup
	killprocess $pid
	exit 1
else
	echo "CHAP authentication failed as expected"
fi

set -e
timing_exit discovery_auth_failure

# This section expects success of CHAP authentication for login to target node.
timing_enter target_auth_success

$rpc_py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:disk1" -d

# Target requires no CHAP. Initiator does not use CHAP.
iscsi-inq iscsi://$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:disk1/0

$rpc_py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:disk1" -g 1

# Target uses CHAP if initiator uses. Initiator does not use CHAP.
iscsi-inq iscsi://$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:disk1/0

# Target uses CHAP if initiator uses. Initiaotor uses one-way CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:disk1/0

$rpc_py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:disk1" -g 2

# Target uses CHAP if initiator uses. Initiator uses mutual CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:disk1/0?target_user=target1\&target_password=tsecret1

$rpc_py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:disk1" -g 1 -r

# Target requires (one-way) CHAP. Initiator uses one-way CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:disk1/0

$rpc_py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:disk1" -g 2 -r -m

# Target requires mutual CHAP. Initiator uses mutual CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:disk1/0?target_user=target1\&target_password=tsecret1

timing_exit target_auth_success

# This section expects failure of CHAP authentication for login to target node.
timing_enter target_auth_failure
set +e

$rpc_py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:Target3" -d

# Target requires no CHAP. Initiaotor uses one-way CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:disk1/0
if [ $? -eq 0 ]; then
	echo "CHAP authentication succeed - expected failure"
	iscsicleanup
	killprocess $pid
	exit 1
else
	echo "CHAP authentication failed as expected"
fi

sudo ./scripts/rpc.py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:disk1" -g 1 -r

# Target requires (one-way) CHAP. Initiator does not use CHAP.
iscsi-inq iscsi://$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:disk1/0
if [ $? -eq 0 ]; then
	echo "CHAP authentication succeed - expected failure"
	iscsicleanup
	killprocess $pid
	exit 1
else
	echo "CHAP authentication failed as expected"
fi

# Target requires (one-way) CHAP. Initiator uses mutual CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:disk1/0?target_user=target1\&target_password=tsecret1
if [ $? -eq 0 ]; then
	echo "CHAP authentication succeed - expected failure"
	iscsicleanup
	killprocess $pid
	exit 1
else
	echo "CHAP authentication failed as expected"
fi

set -e
timing_exit target_auth_failure

trap - SIGINT SIGTERM EXIT

killprocess $pid

report_test_completion "iscsi_initiator"
timing_exit initiator
