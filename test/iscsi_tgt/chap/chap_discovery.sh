#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh
source $rootdir/test/iscsi_tgt/chap/chap_common.sh

USER="chapo"
MUSER="mchapo"
PASS="123456789123"
MPASS="321978654321"

#initialize test:
iscsitestinit
#set up iscsi target
set_up_iscsi_target

#configure target to require bi derectional chap authentication for discovery - mutual credentials:
echo "configuring target for bideerctional authentication"
config_chap_credentials_for_target -t 1 -u $USER -s $PASS -r $MUSER -m $MPASS -d -b
echo "executing discovery without adding credential to initiator - we expect failure"
rc=0
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT || rc=$?
if [ $rc -eq 0 ]; then
	echo "[ERROR] - we are not allowed to discover targets without providing credentials"
	exit 1
fi

#configure  initiator credentials:
echo "configuring initiator for bideerctional authentication"
config_chap_credentials_for_initiator -t 1 -u $USER -s $PASS -r $MUSER -m $MPASS -d -b
echo "executing discovery with adding credential to initiator"
rc=0
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT || rc=$?
if [ $rc -ne 0 ]; then
	echo "[ERROR] - now that we have credentials - we should be able to discover the target"
	exit 1
fi
echo "DONE"
default_initiator_chap_credentials

trap - SIGINT SIGTERM EXIT

killprocess $pid

iscsitestfini
