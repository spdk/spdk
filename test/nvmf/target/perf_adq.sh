#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

gather_supported_nvmf_pci_devs
TCP_INTERFACE_LIST=("${net_devs[@]}")
if ((${#TCP_INTERFACE_LIST[@]} == 0)); then
	echo "ERROR: Physical TCP interfaces are not ready"
	exit 1
fi

perf="$SPDK_EXAMPLE_DIR/perf"

function adq_configure_driver() {
	# Enable adding flows to hardware
	"${NVMF_TARGET_NS_CMD[@]}" ethtool --offload $NVMF_TARGET_INTERFACE hw-tc-offload on
	# ADQ driver turns on this switch by default, we need to turn it off for SPDK testing
	"${NVMF_TARGET_NS_CMD[@]}" ethtool --set-priv-flags $NVMF_TARGET_INTERFACE channel-pkt-inspect-optimize off
	# Since sockets are non-blocking, a non-zero value of net.core.busy_read is sufficient
	sysctl -w net.core.busy_poll=1
	sysctl -w net.core.busy_read=1

	tc=/usr/sbin/tc
	# Create 2 traffic classes and 2 tc1 queues
	"${NVMF_TARGET_NS_CMD[@]}" $tc qdisc add dev $NVMF_TARGET_INTERFACE root \
		mqprio num_tc 2 map 0 1 queues 2@0 2@2 hw 1 mode channel
	"${NVMF_TARGET_NS_CMD[@]}" $tc qdisc add dev $NVMF_TARGET_INTERFACE ingress
	# TC filter is configured using target address (traddr) and port number (trsvcid) to steer packets
	"${NVMF_TARGET_NS_CMD[@]}" $tc filter add dev $NVMF_TARGET_INTERFACE protocol \
		ip parent ffff: prio 1 flower dst_ip $NVMF_FIRST_TARGET_IP/32 ip_proto tcp dst_port $NVMF_PORT skip_sw hw_tc 1
	# Setup mechanism for Tx queue selection based on Rx queue(s) map
	"${NVMF_TARGET_NS_CMD[@]}" $rootdir/scripts/perf/nvmf/set_xps_rxqs $NVMF_TARGET_INTERFACE
}

function adq_configure_nvmf_target() {
	$rpc_py sock_impl_set_options --enable-placement-id $1 --enable-zerocopy-send-server -i posix
	$rpc_py framework_start_init
	$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS --io-unit-size 8192 --sock-priority $1
	$rpc_py bdev_malloc_create 64 512 -b Malloc1
	$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
	$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Malloc1
	$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
}

function adq_reload_driver() {
	rmmod ice
	modprobe ice
	sleep 5
}

# Clear the previous configuration that may have an impact.
# At present, ADQ configuration is only applicable to the ice driver.
adq_reload_driver

# We are going to run the test twice, once with ADQ enabled and once with it disabled.
# The nvmf target is given 4 cores and ADQ creates queues in one traffic class. We then run
# perf with 4 cores (i.e. 4 connections) and examine how the connections are allocated to the nvmf target's
# poll groups.

# When ADQ is disabled, we expect 1 connection on each of the 4 poll groups.
nvmftestinit
nvmfappstart -m 0xF --wait-for-rpc
adq_configure_nvmf_target 0
$perf -q 64 -o 4096 -w randread -t 10 -c 0xF0 \
	-r "trtype:${TEST_TRANSPORT} adrfam:IPv4 traddr:${NVMF_FIRST_TARGET_IP} trsvcid:${NVMF_PORT} \
	subnqn:nqn.2016-06.io.spdk:cnode1" &
perfpid=$!
sleep 2

count=$("$rpc_py" nvmf_get_stats | jq -r '.poll_groups[] | select(.current_io_qpairs == 1) | length' | wc -l)
if [[ "$count" -ne 4 ]]; then
	echo "ERROR: With ADQ disabled, connections were not evenly distributed amongst poll groups!"
	exit 1
fi
wait $perfpid
nvmftestfini

adq_reload_driver

# When ADQ is enabled, we expect the connections to reside on AT MOST two poll groups.
nvmftestinit
adq_configure_driver
nvmfappstart -m 0xF --wait-for-rpc
adq_configure_nvmf_target 1
$perf -q 64 -o 4096 -w randread -t 10 -c 0xF0 \
	-r "trtype:${TEST_TRANSPORT} adrfam:IPv4 traddr:${NVMF_FIRST_TARGET_IP} trsvcid:${NVMF_PORT} \
	subnqn:nqn.2016-06.io.spdk:cnode1" &
perfpid=$!
sleep 2

count=$("$rpc_py" nvmf_get_stats | jq -r '.poll_groups[] | select(.current_io_qpairs == 0) | length' | wc -l)
if [[ "$count" -lt 2 ]]; then
	echo "ERROR: With ADQ enabled, did not find 0 connections on 2 of the poll groups!"
	exit 1
fi

wait $perfpid
nvmftestfini

trap - SIGINT SIGTERM EXIT
