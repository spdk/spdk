#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation
#  All rights reserved.
#

# This test is a regression test for issue #2901

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

if [ "$TEST_TRANSPORT" == "rdma" ]; then
	echo "Skipping tests on RDMA because the rdma stack fails to configure the same IP for host and target."
	exit 0
fi

discovery_port=8009
discovery_nqn=nqn.2014-08.org.nvmexpress.discovery

# nqn prefix to use for subsystem nqns
nqn=nqn.2016-06.io.spdk:cnode

host_nqn=nqn.2021-12.io.spdk:test
host_sock=/tmp/host.sock

function get_bdev_list() {
	$rpc_py -s $host_sock bdev_get_bdevs | jq -r '.[].name' | sort | xargs
}

function wait_for_bdev() {
	while [[ $(get_bdev_list) != "$1" ]]; do
		sleep 1
	done
}

# Start test that check discovery service reconnect ability
nvmftestinit
nvmfappstart -m 0x2

# Start target with single null bdev
$rpc_py << CFG
	log_set_flag bdev_null
	nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
	nvmf_subsystem_add_listener $discovery_nqn -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
		-s $discovery_port
	bdev_null_create null0 1000 512
	nvmf_create_subsystem ${nqn}0 -i 1 -I 100
	nvmf_subsystem_add_ns ${nqn}0 null0 -n 1
	nvmf_subsystem_add_listener ${nqn}0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
		-s $NVMF_PORT
	nvmf_subsystem_add_listener discovery -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
		-s $NVMF_PORT
	nvmf_subsystem_add_host ${nqn}0 $host_nqn
CFG

$SPDK_BIN_DIR/nvmf_tgt -m 0x1 -r $host_sock --wait-for-rpc -L bdev_nvme &
hostpid=$!
waitforlisten $hostpid $host_sock

trap 'process_shm --id $NVMF_APP_SHM_ID; killprocess $hostpid; nvmftestfini; exit 1' SIGINT SIGTERM EXIT

# set TRANSPORT_ACK_TIMEOUT for spdk to recognize disconnections
$rpc_py -s $host_sock bdev_nvme_set_options -e 1
$rpc_py -s $host_sock framework_start_init

# start discovery controller with CTRLR_LOSS_TIMEOUT_SEC
$rpc_py -s $host_sock bdev_nvme_start_discovery -b nvme -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
	-s $discovery_port -f ipv4 -q $host_nqn --ctrlr-loss-timeout-sec 2 \
	--reconnect-delay-sec 1 --fast-io-fail-timeout-sec 1 --wait-for-attach
wait_for_bdev "nvme0n1"

# Delete network interface to trigger reconnection attempts
"${NVMF_TARGET_NS_CMD[@]}" ip addr del $NVMF_FIRST_TARGET_IP/24 dev $NVMF_TARGET_INTERFACE
"${NVMF_TARGET_NS_CMD[@]}" ip link set $NVMF_TARGET_INTERFACE down

# Wait a few sec to ensure that ctrlr is removed and ctrlr_loss_timeout_sec exceeded
wait_for_bdev ""

# Resume network connection
"${NVMF_TARGET_NS_CMD[@]}" ip addr add $NVMF_FIRST_TARGET_IP/24 dev $NVMF_TARGET_INTERFACE
"${NVMF_TARGET_NS_CMD[@]}" ip link set $NVMF_TARGET_INTERFACE up

# Wait some more for discovery controller to reattached bdev
wait_for_bdev "nvme1n1"

trap - SIGINT SIGTERM EXIT

killprocess $hostpid
nvmftestfini
