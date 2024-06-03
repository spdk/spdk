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

#configure target to require chap authentication: add muser+mpassword but dont ask to use it with -m:
echo "configuring target for authentication"
config_chap_credentials_for_target -t 1 -u $USER -s $PASS -r $MUSER -m $MPASS -d -l
echo "executing discovery without adding credential to initiator - we expect failure"
#configure  initiator credentials:
echo "configuring initiator with biderectional authentication"
config_chap_credentials_for_initiator -t 1 -u $USER -s $PASS -r $MUSER -m $MPASS -d -l -b
echo "executing discovery - target should not be discovered since the -m option was not used"
rc=0
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT || rc=$?
if [ $rc -eq 0 ]; then
	echo "[ERROR] - target should not be discovered since the -m option was not used"
	exit 1
fi
echo "configuring target for authentication with the -m option"
config_chap_credentials_for_target -t 2 -u $USER -s $PASS -r $MUSER -m $MPASS -d -l -b
echo "executing discovery:"
rc=0
iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT || rc=$?
if [ $rc -ne 0 ]; then
	echo "[ERROR] - now that we have set the -m option - we should be able to discover."
	exit 1
fi
echo "executing login:"
rc=0
iscsiadm -m node -l -p $TARGET_IP:$ISCSI_PORT || rc=$?
if [ $rc -ne 0 ]; then
	echo "[ERROR] - now that we have set the -m option - we should be able to login."
	exit 1
fi

echo "DONE"
default_initiator_chap_credentials

trap - SIGINT SIGTERM EXIT

killprocess $pid

iscsitestfini
