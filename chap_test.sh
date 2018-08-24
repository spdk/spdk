#!/usr/bin/env bash

TARGET_IP=127.0.0.1
PORTAL_TAG=1
ISCSI_PORT=3260
INITIATOR_TAG=1
INITIATOR_MASK=ANY
NETMASK=ANY

set -e

sudo ./scripts/rpc.py set_iscsi_options -s
sudo ./scripts/rpc.py start_subsystem_init
echo "iscsi_tgt is listening. Running tests..."

sudo ./scripts/rpc.py add_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
sudo ./scripts/rpc.py add_initiator_group $INITIATOR_TAG $INITIATOR_MASK $NETMASK
sudo ./scripts/rpc.py construct_malloc_bdev 64 512

sudo ./scripts/rpc.py construct_target_node Target3 Target3_alias 'Malloc0:0' $PORTAL_TAG:$INITIATOR_TAG 64

sudo ./scripts/rpc.py add_iscsi_auth_group 1 'user:host1 secret:hsecret1'
sudo ./scripts/rpc.py add_iscsi_auth_group 2 'user:host1 secret:hsecret1 muser:target1 msecret:tsecret1'

# This section expects success of CHAP authentication for discovery session.
#timing_enter discovery_auth_success

sudo ./scripts/rpc.py set_iscsi_discovery_auth -d

# Discovery requires no CHAP. Initiator does not use CHAP.
iscsi-ls iscsi://$TARGET_IP:$ISCSI_PORT

sudo ./scripts/rpc.py set_iscsi_discovery_auth -g 1

# Discovery uses CHAP if initiator uses. Initiator does not use CHAP.
iscsi-ls iscsi://$TARGET_IP:$ISCSI_PORT

# Discovery uses CHAP if initiator uses. Initiaotor uses one-way CHAP.
iscsi-ls iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT

sudo ./scripts/rpc.py set_iscsi_discovery_auth -g 2

# Discovery uses CHAP if initiator uses. Initiator uses mutual CHAP.
iscsi-ls iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/?target_user=target1\&target_password=tsecret1

sudo ./scripts/rpc.py set_iscsi_discovery_auth -g 1 -r

# Discovery requires (one-way) CHAP. Initiator uses one-way CHAP.
iscsi-ls iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT

sudo ./scripts/rpc.py set_iscsi_discovery_auth -g 2 -r -m

# Discovery requires mutual CHAP. Initiator uses mutual CHAP.
iscsi-ls iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/?target_user=target1\&target_password=tsecret1

#timing_exit discovery_auth_success

# This section expects failure of CHAP authentication for discovery session.
#timing_enter discovery_auth_failure
set +e

sudo ./scripts/rpc.py set_iscsi_discovery_auth -d

# Discovery requires no CHAP. Initiaotor uses one-way CHAP.
iscsi-ls iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT
if [ $? -eq 0 ]; then
	echo "CHAP authentication succeed - expected failure"
#	iscsicleanup
#	killprocess $pid
	exit 1
else
	echo "CHAP authentication failed as expected"
fi

sudo ./scripts/rpc.py set_iscsi_discovery_auth -g 1 -r

# Discovery requires (one-way) CHAP. Initiator does not use CHAP.
iscsi-ls iscsi://$TARGET_IP:$ISCSI_PORT
if [ $? -eq 0 ]; then
        echo "CHAP authentication succeed - expected failure"
#       iscsicleanup
#       killprocess $pid
        exit 1
else
        echo "CHAP authentication failed as expected"
fi

# Discovery requires (one-way) CHAP. Initiator uses mutual CHAP.
iscsi-ls iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/?target_user=target1\&target_password=tsecret1
if [ $? -eq 0 ]; then
        echo "CHAP authentication succeed - expected failure"
#       iscsicleanup
#       killprocess $pid
        exit 1
else
        echo "CHAP authentication failed as expected"
fi

set -e

#timing_exit discovery_auth_failure

# This section expects success of CHAP authentication for login to target node.
#timing_enter target_auth_success

sudo ./scripts/rpc.py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:Target3" -d

# Target requires no CHAP. Initiator does not use CHAP.
iscsi-inq iscsi://$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0

sudo ./scripts/rpc.py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:Target3" -g 1

# Target uses CHAP if initiator uses. Initiator does not use CHAP.
iscsi-inq iscsi://$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0

# Target uses CHAP if initiator uses. Initiaotor uses one-way CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0

sudo ./scripts/rpc.py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:Target3" -g 2

# Target uses CHAP if initiator uses. Initiator uses mutual CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0?target_user=target1\&target_password=tsecret1

sudo ./scripts/rpc.py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:Target3" -g 1 -r

# Target requires (one-way) CHAP. Initiator uses one-way CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0

sudo ./scripts/rpc.py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:Target3" -g 2 -r -m

# Target requires mutual CHAP. Initiator uses mutual CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0?target_user=target1\&target_password=tsecret1

#timing_exit target_auth_success

# This section expects failure of CHAP authentication for login to target node.
#timing_enter target_auth_failure
set +e

sudo ./scripts/rpc.py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:Target3" -d

# Target requires no CHAP. Initiaotor uses one-way CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0
if [ $? -eq 0 ]; then
        echo "CHAP authentication succeed - expected failure"
#       iscsicleanup
#       killprocess $pid
        exit 1
else
        echo "CHAP authentication failed as expected"
fi

sudo ./scripts/rpc.py set_iscsi_target_node_auth "iqn.2016-06.io.spdk:Target3" -g 1 -r

# Target requires (one-way) CHAP. Initiator does not use CHAP.
iscsi-inq iscsi://$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0
if [ $? -eq 0 ]; then
        echo "CHAP authentication succeed - expected failure"
#       iscsicleanup
#       killprocess $pid
        exit 1
else
        echo "CHAP authentication failed as expected"
fi

# Target requires (one-way) CHAP. Initiator uses mutual CHAP.
iscsi-inq iscsi://host1%hsecret1@$TARGET_IP:$ISCSI_PORT/iqn.2016-06.io.spdk:Target3/0?target_user=target1\&target_password=tsecret1
if [ $? -eq 0 ]; then
        echo "CHAP authentication succeed - expected failure"
#       iscsicleanup
#       killprocess $pid
        exit 1
else
        echo "CHAP authentication failed as expected"
fi

set -e
