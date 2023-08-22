#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

NULL_BDEV_SIZE=102400
NULL_BLOCK_SIZE=512
NVMF_REFERRAL_IP_1=127.0.0.2
NVMF_REFERRAL_IP_2=127.0.0.3
NVMF_REFERRAL_IP_3=127.0.0.4
NVMF_PORT_REFERRAL=4430

if ! hash nvme; then
	echo "nvme command not found; skipping referrals test"
	exit 0
fi

function get_referral_ips() {
	$rpc_py nvmf_discovery_get_referrals | jq -r '.[].address.traddr' | sort | xargs
}

nvmftestinit
nvmfappstart -m 0xF

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

# Add a referral to another discovery service
$rpc_py nvmf_discovery_add_referral -t $TEST_TRANSPORT -a $NVMF_REFERRAL_IP_1 -s $NVMF_PORT_REFERRAL
$rpc_py nvmf_discovery_add_referral -t $TEST_TRANSPORT -a $NVMF_REFERRAL_IP_2 -s $NVMF_PORT_REFERRAL
$rpc_py nvmf_discovery_add_referral -t $TEST_TRANSPORT -a $NVMF_REFERRAL_IP_3 -s $NVMF_PORT_REFERRAL

if [ "$($rpc_py nvmf_discovery_get_referrals | jq 'length')" != "3" ]; then
	echo "nvmf_discovery_add_referral RPC didn't properly create referrals." && false
fi

[[ $(get_referral_ips) == "$NVMF_REFERRAL_IP_1 $NVMF_REFERRAL_IP_2 $NVMF_REFERRAL_IP_3" ]]

$rpc_py nvmf_discovery_remove_referral -t $TEST_TRANSPORT -a $NVMF_REFERRAL_IP_1 -s $NVMF_PORT_REFERRAL
$rpc_py nvmf_discovery_remove_referral -t $TEST_TRANSPORT -a $NVMF_REFERRAL_IP_2 -s $NVMF_PORT_REFERRAL
$rpc_py nvmf_discovery_remove_referral -t $TEST_TRANSPORT -a $NVMF_REFERRAL_IP_3 -s $NVMF_PORT_REFERRAL

if [ "$($rpc_py nvmf_discovery_get_referrals | jq 'length')" != "0" ]; then
	echo "nvmf_discovery_remove_referral RPC didn't properly remove referrals." && false
fi

trap - SIGINT SIGTERM EXIT

nvmftestfini
