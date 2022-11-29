#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

if [ "$TEST_TRANSPORT" == "rdma" ]; then
	echo "Skipping tests on RDMA because the rdma stack fails to configure the same IP for host and target."
	exit 0
fi

DISCOVERY_PORT=8009
DISCOVERY_NQN=nqn.2014-08.org.nvmexpress.discovery

# NQN prefix to use for subsystem NQNs
NQN=nqn.2016-06.io.spdk:cnode

HOST_NQN=nqn.2021-12.io.spdk:test
HOST_SOCK=/tmp/host.sock

nvmftestinit

# We will start the target as normal, emulating a storage cluster. We will simulate new paths
# to the cluster via multiple listeners with different TCP ports.

nvmfappstart -m 0x2

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
$rpc_py nvmf_subsystem_add_listener $DISCOVERY_NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
	-s $DISCOVERY_PORT
$rpc_py bdev_null_create null0 1000 512
$rpc_py bdev_null_create null1 1000 512
$rpc_py bdev_wait_for_examine

# Now start the host where the discovery service will run.  For our tests, we will send RPCs to
# the "cluster" to create subsystems, add namespaces, add and remove listeners and add hosts,
# and then check if the discovery service has detected the changes and constructed the correct
# subsystem, ctrlr and bdev objects.

$SPDK_BIN_DIR/nvmf_tgt -m 0x1 -r $HOST_SOCK &
hostpid=$!
waitforlisten $hostpid $HOST_SOCK

trap 'process_shm --id $NVMF_APP_SHM_ID; kill $hostpid; nvmftestfini; exit 1' SIGINT SIGTERM EXIT

$rpc_py -s $HOST_SOCK log_set_flag bdev_nvme
$rpc_py -s $HOST_SOCK bdev_nvme_start_discovery -b nvme -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
	-s $DISCOVERY_PORT -f ipv4 -q $HOST_NQN

function get_bdev_list() {
	$rpc_py -s $HOST_SOCK bdev_get_bdevs | jq -r '.[].name' | sort | xargs
}

function get_subsystem_names() {
	$rpc_py -s $HOST_SOCK bdev_nvme_get_controllers | jq -r '.[].name' | sort | xargs
}

function get_subsystem_paths() {
	$rpc_py -s $HOST_SOCK bdev_nvme_get_controllers -n $1 | jq -r '.[].ctrlrs[].trid.trsvcid' | sort -n | xargs
}

function get_discovery_ctrlrs() {
	$rpc_py -s $HOST_SOCK bdev_nvme_get_discovery_info | jq -r '.[].name' | sort | xargs
}

# Note that tests need to call get_notification_count and then check $notification_count,
# because if we use $(get_notification_count), the notify_id gets updated in the subshell.
notify_id=0
function get_notification_count() {
	notification_count=$($rpc_py -s $HOST_SOCK notify_get_notifications -i $notify_id | jq '. | length')
	notify_id=$((notify_id + notification_count))
}

[[ "$(get_subsystem_names)" == "" ]]
[[ "$(get_bdev_list)" == "" ]]

$rpc_py nvmf_create_subsystem ${NQN}0
[[ "$(get_subsystem_names)" == "" ]]
[[ "$(get_bdev_list)" == "" ]]

$rpc_py nvmf_subsystem_add_ns ${NQN}0 null0
[[ "$(get_subsystem_names)" == "" ]]
[[ "$(get_bdev_list)" == "" ]]

# Add listener for the subsystem.  But since the the subsystem was not created with -a option, the
# discovery host will not be able to see the subsystem until its hostnqn is added.
$rpc_py nvmf_subsystem_add_listener ${NQN}0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
[[ "$(get_subsystem_names)" == "" ]]
[[ "$(get_bdev_list)" == "" ]]
get_notification_count
[[ $notification_count == 0 ]]

# Discovery hostnqn is added, so now the host should see the subsystem, with a single path for the
# port of the single listener on the target.
$rpc_py nvmf_subsystem_add_host ${NQN}0 $HOST_NQN
sleep 1 # Wait a bit to make sure the discovery service has a chance to detect the changes
[[ "$(get_subsystem_names)" == "nvme0" ]]
[[ "$(get_bdev_list)" == "nvme0n1" ]]
[[ "$(get_subsystem_paths nvme0)" == "$NVMF_PORT" ]]
get_notification_count
[[ $notification_count == 1 ]]

# Adding a namespace isn't a discovery function, but do it here anyways just to confirm we see a new bdev.
$rpc_py nvmf_subsystem_add_ns ${NQN}0 null1
sleep 1 # Wait a bit to make sure the discovery service has a chance to detect the changes
[[ "$(get_bdev_list)" == "nvme0n1 nvme0n2" ]]
get_notification_count
[[ $notification_count == 1 ]]

# Add a second path to the same subsystem.  This shouldn't change the list of subsystems or bdevs, but
# we should see a second path on the nvme0 subsystem now.
$rpc_py nvmf_subsystem_add_listener ${NQN}0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT
sleep 1 # Wait a bit to make sure the discovery service has a chance to detect the changes
[[ "$(get_subsystem_names)" == "nvme0" ]]
[[ "$(get_bdev_list)" == "nvme0n1 nvme0n2" ]]
[[ "$(get_subsystem_paths nvme0)" == "$NVMF_PORT $NVMF_SECOND_PORT" ]]
get_notification_count
[[ $notification_count == 0 ]]

# Remove the listener for the first port.  The subsystem and bdevs should stay, but we should see
# the path to that first port disappear.
$rpc_py nvmf_subsystem_remove_listener ${NQN}0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
sleep 1 # Wait a bit to make sure the discovery service has a chance to detect the changes
[[ "$(get_subsystem_names)" == "nvme0" ]]
[[ "$(get_bdev_list)" == "nvme0n1 nvme0n2" ]]
[[ "$(get_subsystem_paths nvme0)" == "$NVMF_SECOND_PORT" ]]
get_notification_count
[[ $notification_count == 0 ]]

$rpc_py -s $HOST_SOCK bdev_nvme_stop_discovery -b nvme
sleep 1 # Wait a bit to make sure the discovery service has a chance to detect the changes
[[ "$(get_subsystem_names)" == "" ]]
[[ "$(get_bdev_list)" == "" ]]
get_notification_count
[[ $notification_count == 2 ]]

# Make sure that it's not possible to start two discovery services with the same name
$rpc_py -s $HOST_SOCK bdev_nvme_start_discovery -b nvme -t $TEST_TRANSPORT \
	-a $NVMF_FIRST_TARGET_IP -s $DISCOVERY_PORT -f ipv4 -q $HOST_NQN -w
NOT $rpc_py -s $HOST_SOCK bdev_nvme_start_discovery -b nvme -t $TEST_TRANSPORT \
	-a $NVMF_FIRST_TARGET_IP -s $DISCOVERY_PORT -f ipv4 -q $HOST_NQN -w
[[ $(get_discovery_ctrlrs) == "nvme" ]]
[[ $(get_bdev_list) == "nvme0n1 nvme0n2" ]]

# Make sure that it's also impossible to start the discovery using the same trid
NOT $rpc_py -s $HOST_SOCK bdev_nvme_start_discovery -b nvme_second -t $TEST_TRANSPORT \
	-a $NVMF_FIRST_TARGET_IP -s $DISCOVERY_PORT -f ipv4 -q $HOST_NQN -w
[[ $(get_discovery_ctrlrs) == "nvme" ]]
[[ $(get_bdev_list) == "nvme0n1 nvme0n2" ]]

# Try to connect to a non-existing discovery endpoint and verify that it'll timeout
NOT $rpc_py -s $HOST_SOCK bdev_nvme_start_discovery -b nvme_second -t $TEST_TRANSPORT \
	-a $NVMF_FIRST_TARGET_IP -s $((DISCOVERY_PORT + 1)) -f ipv4 -q $HOST_NQN -T 3000
[[ $(get_discovery_ctrlrs) == "nvme" ]]

trap - SIGINT SIGTERM EXIT

kill $hostpid
nvmftestfini
