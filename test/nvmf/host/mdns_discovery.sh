#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries. All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

DISCOVERY_FILTER="address"
DISCOVERY_PORT=8009
DISCOVERY_NQN=nqn.2014-08.org.nvmexpress.discovery

# NQN prefix to use for subsystem NQNs
NQN=nqn.2016-06.io.spdk:cnode
NQN2=nqn.2016-06.io.spdk:cnode2

HOST_NQN=nqn.2021-12.io.spdk:test
HOST_SOCK=/tmp/host.sock

nvmftestinit

# We will start the target as normal, emulating a storage cluster. We will simulate new paths
# to the cluster via multiple listeners with different TCP ports.

nvmfappstart -m 0x2 --wait-for-rpc

$rpc_py nvmf_set_config --discovery-filter=$DISCOVERY_FILTER
$rpc_py framework_start_init
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192
$rpc_py nvmf_subsystem_add_listener $DISCOVERY_NQN -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP \
	-s $DISCOVERY_PORT
$rpc_py bdev_null_create null0 1000 512
$rpc_py bdev_null_create null1 1000 512
$rpc_py bdev_null_create null2 1000 512
$rpc_py bdev_null_create null3 1000 512
$rpc_py bdev_wait_for_examine

# Now start the host where the discovery service will run.  For our tests, we will send RPCs to
# the "cluster" to create subsystems, add namespaces, add and remove listeners and add hosts,
# and then check if the discovery service has detected the changes and constructed the correct
# subsystem, ctrlr and bdev objects.

$SPDK_BIN_DIR/nvmf_tgt -m 0x1 -r $HOST_SOCK &
hostpid=$!
waitforlisten $hostpid $HOST_SOCK

trap 'process_shm --id $NVMF_APP_SHM_ID;exit 1' SIGINT SIGTERM
trap 'process_shm --id $NVMF_APP_SHM_ID;nvmftestfini;kill $hostpid;kill $avahi_clientpid;kill $avahipid;' EXIT

# Make sure any existing avahi-daemon is killed before we start it with the specified
# configuration file limiting it to the NVMF_TARGET_INTERFACE.
avahi-daemon --kill || :
"${NVMF_TARGET_NS_CMD[@]}" avahi-daemon -f <(echo -e "[server]\nallow-interfaces=$NVMF_TARGET_INTERFACE,$NVMF_TARGET_INTERFACE2\nuse-ipv4=yes\nuse-ipv6=no") &
avahipid=$!
sleep 1

$rpc_py -s $HOST_SOCK log_set_flag bdev_nvme
$rpc_py -s $HOST_SOCK bdev_nvme_start_mdns_discovery -b mdns -s _nvme-disc._tcp -q $HOST_NQN

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

function get_mdns_discovery_svcs() {
	$rpc_py -s $HOST_SOCK bdev_nvme_get_mdns_discovery_info | jq -r '.[].name' | sort | xargs
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

# Discovery hostnqn is added, so now the host should see the subsystem, with a single path for the
# port of the single listener on the target.
$rpc_py nvmf_subsystem_add_host ${NQN}0 $HOST_NQN

# Adding the second discovery controller and target
$rpc_py nvmf_create_subsystem ${NQN2}0
$rpc_py nvmf_subsystem_add_ns ${NQN2}0 null2

# Discovery hostnqn is added, so now the host should see the subsystem, with a single path for the
# port of the single listener on the target.
$rpc_py nvmf_subsystem_add_host ${NQN2}0 $HOST_NQN

$rpc_py nvmf_subsystem_add_listener $DISCOVERY_NQN -t $TEST_TRANSPORT -a $NVMF_SECOND_TARGET_IP \
	-s $DISCOVERY_PORT
$rpc_py nvmf_subsystem_add_listener ${NQN2}0 -t $TEST_TRANSPORT -a $NVMF_SECOND_TARGET_IP -s $NVMF_PORT

#Simulate discovery service publishing by the target
"${NVMF_TARGET_NS_CMD[@]}" /usr/bin/avahi-publish --domain=local --service CDC _nvme-disc._tcp $DISCOVERY_PORT "NQN=$DISCOVERY_NQN" "p=tcp" &
avahi_clientpid=$!
sleep 5 # Wait a bit to make sure the discovery service has a chance to detect the changes

[[ "$(get_mdns_discovery_svcs)" == "mdns" ]]
[[ $(get_discovery_ctrlrs) == "mdns0_nvme mdns1_nvme" ]]
[[ "$(get_subsystem_names)" == "mdns0_nvme0 mdns1_nvme0" ]]
[[ "$(get_bdev_list)" == "mdns0_nvme0n1 mdns1_nvme0n1" ]]
[[ "$(get_subsystem_paths mdns0_nvme0)" == "$NVMF_PORT" ]]
[[ "$(get_subsystem_paths mdns1_nvme0)" == "$NVMF_PORT" ]]
get_notification_count
[[ $notification_count == 2 ]]

# Adding a namespace isn't a discovery function, but do it here anyways just to confirm we see a new bdev.
$rpc_py nvmf_subsystem_add_ns ${NQN}0 null1
$rpc_py nvmf_subsystem_add_ns ${NQN2}0 null3
sleep 1 # Wait a bit to make sure the discovery service has a chance to detect the changes

[[ "$(get_bdev_list)" == "mdns0_nvme0n1 mdns0_nvme0n2 mdns1_nvme0n1 mdns1_nvme0n2" ]]
get_notification_count
[[ $notification_count == 2 ]]

# Add a second path to the same subsystems.  This shouldn't change the list of subsystems or bdevs, but
# we should see a second path on the mdns0_nvme0 and mdns1_nvme0  subsystems now.
$rpc_py nvmf_subsystem_add_listener ${NQN}0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_SECOND_PORT
$rpc_py nvmf_subsystem_add_listener ${NQN2}0 -t $TEST_TRANSPORT -a $NVMF_SECOND_TARGET_IP -s $NVMF_SECOND_PORT
sleep 1 # Wait a bit to make sure the discovery service has a chance to detect the changes

[[ "$(get_subsystem_names)" == "mdns0_nvme0 mdns1_nvme0" ]]
[[ "$(get_bdev_list)" == "mdns0_nvme0n1 mdns0_nvme0n2 mdns1_nvme0n1 mdns1_nvme0n2" ]]
[[ "$(get_subsystem_paths mdns0_nvme0)" == "$NVMF_PORT $NVMF_SECOND_PORT" ]]
[[ "$(get_subsystem_paths mdns1_nvme0)" == "$NVMF_PORT $NVMF_SECOND_PORT" ]]
get_notification_count
[[ $notification_count == 0 ]]

# Remove the listener for the first port.  The subsystem and bdevs should stay, but we should see
# the path to that first port disappear.
$rpc_py nvmf_subsystem_remove_listener ${NQN}0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$rpc_py nvmf_subsystem_remove_listener ${NQN2}0 -t $TEST_TRANSPORT -a $NVMF_SECOND_TARGET_IP -s $NVMF_PORT
sleep 1 # Wait a bit to make sure the discovery service has a chance to detect the changes

[[ "$(get_subsystem_names)" == "mdns0_nvme0 mdns1_nvme0" ]]
[[ "$(get_bdev_list)" == "mdns0_nvme0n1 mdns0_nvme0n2 mdns1_nvme0n1 mdns1_nvme0n2" ]]
[[ "$(get_subsystem_paths mdns0_nvme0)" == "$NVMF_SECOND_PORT" ]]
[[ "$(get_subsystem_paths mdns1_nvme0)" == "$NVMF_SECOND_PORT" ]]
get_notification_count
[[ $notification_count == 0 ]]

$rpc_py -s $HOST_SOCK bdev_nvme_stop_mdns_discovery -b mdns
sleep 1 # Wait a bit to make sure the discovery service has a chance to detect the changes

[[ "$(get_mdns_discovery_svcs)" == "" ]]
[[ "$(get_subsystem_names)" == "" ]]
[[ "$(get_bdev_list)" == "" ]]
get_notification_count
[[ $notification_count == 4 ]]

# Make sure that it's impossible to start the discovery using the same bdev name
$rpc_py -s $HOST_SOCK bdev_nvme_start_mdns_discovery -b mdns -s _nvme-disc._tcp -q $HOST_NQN
NOT $rpc_py -s $HOST_SOCK bdev_nvme_start_mdns_discovery -b mdns -s _nvme-disc._http -q $HOST_NQN
sleep 5

[[ "$(get_mdns_discovery_svcs)" == "mdns" ]]
[[ $(get_discovery_ctrlrs) == "mdns0_nvme mdns1_nvme" ]]
[[ $(get_bdev_list) == "mdns0_nvme0n1 mdns0_nvme0n2 mdns1_nvme0n1 mdns1_nvme0n2" ]]

# Make sure that it's also impossible to start the discovery using the same service name
NOT $rpc_py -s $HOST_SOCK bdev_nvme_start_mdns_discovery -b cdc -s _nvme-disc._tcp -q $HOST_NQN
[[ $(get_discovery_ctrlrs) == "mdns0_nvme mdns1_nvme" ]]
[[ $(get_bdev_list) == "mdns0_nvme0n1 mdns0_nvme0n2 mdns1_nvme0n1 mdns1_nvme0n2" ]]
$rpc_py -s $HOST_SOCK bdev_nvme_stop_mdns_discovery -b mdns

trap - SIGINT SIGTERM EXIT

kill $hostpid
# Now wait for $hostpid to exit, otherwise if it's still running when we try to kill $avahipid, avahi
# will auto-restart.
wait $hostpid
kill $avahi_clientpid
kill $avahipid
nvmftestfini
