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
DISCOVERY_NQN=nqn.2014-08.org.nvmexpress.discovery
NQN=nqn.2016-06.io.spdk:cnode1

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

get_discovery_entries() {
	local subtype="$1"

	nvme discover "${NVME_HOST[@]}" -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s 8009 -o json \
		| jq ".records[] | select(.subtype == \"$subtype\")"
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

# Add a referral to a discovery and NVMe subsystems on the same IP/port
rpc_cmd nvmf_discovery_add_referral -t $TEST_TRANSPORT -a $NVMF_REFERRAL_IP_1 \
	-s $NVMF_PORT_REFERRAL -n discovery
rpc_cmd nvmf_discovery_add_referral -t $TEST_TRANSPORT -a $NVMF_REFERRAL_IP_1 \
	-s $NVMF_PORT_REFERRAL -n "$NQN"

[[ $(get_referral_ips "rpc") == "$NVMF_REFERRAL_IP_1 $NVMF_REFERRAL_IP_1" ]]
[[ $(get_referral_ips "nvme") == "$NVMF_REFERRAL_IP_1 $NVMF_REFERRAL_IP_1" ]]
[[ $(get_discovery_entries "nvme subsystem" | jq -r '.subnqn') == "$NQN" ]]
[[ $(get_discovery_entries "discovery subsystem referral" | jq -r '.subnqn') == "$DISCOVERY_NQN" ]]

# Remove one of the referrals and check that it's gone
rpc_cmd nvmf_discovery_remove_referral -t $TEST_TRANSPORT -a $NVMF_REFERRAL_IP_1 \
	-s $NVMF_PORT_REFERRAL -n $NQN
[[ $(get_referral_ips "rpc") == "$NVMF_REFERRAL_IP_1" ]]
[[ $(get_referral_ips "nvme") == "$NVMF_REFERRAL_IP_1" ]]
[[ $(get_discovery_entries "nvme subsystem" | jq -r '.subnqn') == "" ]]
[[ $(get_discovery_entries "discovery subsystem referral" | jq -r '.subnqn') == "$DISCOVERY_NQN" ]]

# Remove the second one
rpc_cmd nvmf_discovery_remove_referral -t $TEST_TRANSPORT -a $NVMF_REFERRAL_IP_1 \
	-s $NVMF_PORT_REFERRAL -n $DISCOVERY_NQN

(($(rpc_cmd nvmf_discovery_get_referrals | jq 'length') == 0))
[[ $(get_referral_ips "nvme") == "" ]]

trap - SIGINT SIGTERM EXIT
nvmftestfini
