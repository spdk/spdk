#!/usr/bin/env bash

# The following test uses libiscsi and its utilities iscsi-ls and iscsi-inq.
# In libiscsi CHAP authentication can be specified via the command line URL
# and it is very convenient. Hence use libiscsi to test CHAP authentication.
#
# iscsi-ls is the utility to list iSCSI targets and LUNs and it is used to
# test CHAP authentication for discovery session.
#
# iscsi-inq is the utility to request INQUIRY data from an iSCSI and LUN and
# it is used to test CHAP authentication for login to iSCSI target node.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

timing_enter chap
set -e

CHAP_USER=host_user
CHAP_SECRET=host_secret
MUTUAL_CHAP_USER=target_user
MUTUAL_CHAP_SECRET=target_secret

rpc_py="python $rootdir/scripts/rpc.py"

timing_enter start_iscsi_tgt

$ISCSI_APP -m $ISCSI_TEST_CORE_MASK -w &
pid=$!
echo "Process pid: $pid"

trap "killproces $pid; exit 1" SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py set_iscsi_options -s
$rpc_py start_subsystem_init
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py add_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
$rpc_py construct_malloc_bdev 64 512

$rpc_py construct_target_node Target3 Target3_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 64

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

$rpc_py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:Target3" -d

# Target requires no CHAP. Initiator does not use CHAP.
iscsi-inq iscsi://$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0

$rpc_py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:Target3" -g 1

# Target uses CHAP if initiator uses. Initiator does not use CHAP.
iscsi-inq iscsi://$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0

# Target uses CHAP if initiator uses. Initiaotor uses one-way CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0

$rpc_py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:Target3" -g 2

# Target uses CHAP if initiator uses. Initiator uses mutual CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0?target_user=target1\&target_password=tsecret1

$rpc_py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:Target3" -g 1 -r

# Target requires (one-way) CHAP. Initiator uses one-way CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0

$rpc_py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:Target3" -g 2 -r -m

# Target requires mutual CHAP. Initiator uses mutual CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0?target_user=target1\&target_password=tsecret1

timing_exit target_auth_success

# This section expects failure of CHAP authentication for login to target node.
timing_enter target_auth_failure
set +e

$rpc_py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:Target3" -d

# Target requires no CHAP. Initiaotor uses one-way CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0
if [ $? -eq 0 ]; then
	echo "CHAP authentication succeed - expected failure"
	iscsicleanup
	killprocess $pid
	exit 1
else
	echo "CHAP authentication failed as expected"
fi

sudo ./scripts/rpc.py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:Target3" -g 1 -r

# Target requires (one-way) CHAP. Initiator does not use CHAP.
iscsi-inq iscsi://$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0
if [ $? -eq 0 ]; then
	echo "CHAP authentication succeed - expected failure"
	iscsicleanup
	killprocess $pid
	exit 1
else
	echo "CHAP authentication failed as expected"
fi

# Target requires (one-way) CHAP. Initiator uses mutual CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0?target_user=target1\&target_password=tsecret1
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

iscsicleanup
killprocess $pid
timing_exit chap
