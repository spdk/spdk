#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

NVMF_REFERRAL_IP_1=127.0.0.2
NVMF_REFERRAL_IP_2=127.0.0.3
NVMF_REFERRAL_IP_3=127.0.0.4
NVMF_PORT_REFERRAL=4430

get_referral_ips() {
	if [[ "$1" == "rpc" ]]; then
		# shellcheck disable=SC2005
		echo $(rpc_cmd nvmf_discovery_get_referrals | jq -r '.[].address.traddr' | sort)
	elif [[ "$1" == "nvme" ]]; then
		# shellcheck disable=SC2005
		echo $(nvme discover "${NVME_HOST[@]}" -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s 8009 -o json \
			| jq -r '.records[] | select(.subtype != "current discovery subsystem").traddr' \
			| sort)
	fi
}

nvmftestinit
nvmfappstart -m 0xF

rpc_cmd nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
rpc_cmd nvmf_subsystem_add_listener -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s 8009 discovery

# Add a referral to another discovery service
rpc_cmd nvmf_discovery_add_referral -t $TEST_TRANSPORT -a $NVMF_REFERRAL_IP_1 -s $NVMF_PORT_REFERRAL
rpc_cmd nvmf_discovery_add_referral -t $TEST_TRANSPORT -a $NVMF_REFERRAL_IP_2 -s $NVMF_PORT_REFERRAL
rpc_cmd nvmf_discovery_add_referral -t $TEST_TRANSPORT -a $NVMF_REFERRAL_IP_3 -s $NVMF_PORT_REFERRAL

(($(rpc_cmd nvmf_discovery_get_referrals | jq 'length') == 3))
[[ $(get_referral_ips "rpc") == "$NVMF_REFERRAL_IP_1 $NVMF_REFERRAL_IP_2 $NVMF_REFERRAL_IP_3" ]]
[[ $(get_referral_ips "nvme") == "$NVMF_REFERRAL_IP_1 $NVMF_REFERRAL_IP_2 $NVMF_REFERRAL_IP_3" ]]

rpc_cmd nvmf_discovery_remove_referral -t $TEST_TRANSPORT -a $NVMF_REFERRAL_IP_1 -s $NVMF_PORT_REFERRAL
rpc_cmd nvmf_discovery_remove_referral -t $TEST_TRANSPORT -a $NVMF_REFERRAL_IP_2 -s $NVMF_PORT_REFERRAL
rpc_cmd nvmf_discovery_remove_referral -t $TEST_TRANSPORT -a $NVMF_REFERRAL_IP_3 -s $NVMF_PORT_REFERRAL

(($(rpc_cmd nvmf_discovery_get_referrals | jq 'length') == 0))
[[ $(get_referral_ips "nvme") == "" ]]

trap - SIGINT SIGTERM EXIT
nvmftestfini
