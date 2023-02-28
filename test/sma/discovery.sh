#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../..")

source "$rootdir/test/common/autotest_common.sh"
source "$testdir/common.sh"

sma_py="$rootdir/scripts/sma-client.py"
rpc_py="$rootdir/scripts/rpc.py"

t1sock='/var/tmp/spdk.sock1'
t2sock='/var/tmp/spdk.sock2'
invalid_port=8008
t1dscport=8009
t2dscport1=8010
t2dscport2=8011
t1nqn='nqn.2016-06.io.spdk:node1'
t2nqn='nqn.2016-06.io.spdk:node2'
hostnqn='nqn.2016-06.io.spdk:host0'
cleanup_period=1

function cleanup() {
	killprocess $smapid
	killprocess $tgtpid
	killprocess $t1pid
	killprocess $t2pid
}

function format_endpoints() {
	local eps=("$@")
	for ((i = 0; i < ${#eps[@]}; i++)); do
		cat <<- EOF
			{
				"trtype": "tcp",
				"traddr": "127.0.0.1",
				"trsvcid": "${eps[i]}"
			}
		EOF
		if ! ((i + 1 == ${#@})); then
			echo ,
		fi
	done
}

function format_volume() {
	local volume_id=$1
	shift

	cat <<- EOF
		"volume": {
			"volume_id": "$(uuid2base64 $volume_id)",
			"nvmf": {
				"hostnqn": "$hostnqn",
				"discovery": {
					"discovery_endpoints": [
						$(format_endpoints "$@")
					]
				}
			}
		}
	EOF
}

function create_device() {
	local nqn=$1
	local volume_id=$2
	local volume=

	shift
	if [[ -n "$volume_id" ]]; then
		volume="$(format_volume "$@"),"
	fi

	$sma_py <<- EOF
		{
			"method": "CreateDevice",
			"params": {
				$volume
				"nvmf_tcp": {
					"subnqn": "$nqn",
					"adrfam": "ipv4",
					"traddr": "127.0.0.1",
					"trsvcid": "4419"
				}
			}
		}
	EOF
}

function delete_device() {
	$sma_py <<- EOF
		{
			"method": "DeleteDevice",
			"params": {
				"handle": "$1"
			}
		}
	EOF
}

function attach_volume() {
	local device_id=$1

	shift
	$sma_py <<- EOF
		{
			"method": "AttachVolume",
			"params": {
				$(format_volume "$@"),
				"device_handle": "$device_id"
			}
		}
	EOF
}

function detach_volume() {
	$sma_py <<- EOF
		{
			"method": "DetachVolume",
			"params": {
				"device_handle": "$1",
				"volume_id": "$(uuid2base64 $2)"
			}
		}
	EOF
}

trap "cleanup; exit 1" SIGINT SIGTERM EXIT

# Start two remote targets
$rootdir/build/bin/spdk_tgt -r $t1sock -m 0x1 &
t1pid=$!
$rootdir/build/bin/spdk_tgt -r $t2sock -m 0x2 &
t2pid=$!

# One target that the SMA will configure
$rootdir/build/bin/spdk_tgt -m 0x4 &
tgtpid=$!

# And finally the SMA itself
$rootdir/scripts/sma.py -c <(
	cat <<- EOF
		discovery_timeout: 5
		volume_cleanup_period: $cleanup_period
		devices:
		  - name: 'nvmf_tcp'
	EOF
) &
smapid=$!

waitforlisten $t1pid
waitforlisten $t2pid

# Prepare the targets.  The first one has a single subsystem with a single volume and a single
# discovery listener.  The second one also has a single subsystem, but has two volumes attached to
# it and has two discovery listeners.
t1uuid=$(uuidgen)
t2uuid=$(uuidgen)
t2uuid2=$(uuidgen)

$rpc_py -s $t1sock <<- EOF
	nvmf_create_transport -t tcp
	bdev_null_create null0 128 4096 -u $t1uuid
	nvmf_create_subsystem $t1nqn -s SPDK00000000000001 -d SPDK_Controller1
	nvmf_subsystem_add_host $t1nqn $hostnqn
	nvmf_subsystem_add_ns $t1nqn $t1uuid
	nvmf_subsystem_add_listener $t1nqn -t tcp -a 127.0.0.1 -s 4420
	nvmf_subsystem_add_listener discovery -t tcp -a 127.0.0.1 -s $t1dscport
EOF

$rpc_py -s $t2sock <<- EOF
	nvmf_create_transport -t tcp
	bdev_null_create null0 128 4096 -u $t2uuid
	bdev_null_create null1 128 4096 -u $t2uuid2
	nvmf_create_subsystem $t2nqn -s SPDK00000000000001 -d SPDK_Controller1
	nvmf_subsystem_add_host $t2nqn $hostnqn
	nvmf_subsystem_add_ns $t2nqn $t2uuid
	nvmf_subsystem_add_ns $t2nqn $t2uuid2
	nvmf_subsystem_add_listener $t2nqn -t tcp -a 127.0.0.1 -s 4421
	nvmf_subsystem_add_listener discovery -t tcp -a 127.0.0.1 -s $t2dscport1
	nvmf_subsystem_add_listener discovery -t tcp -a 127.0.0.1 -s $t2dscport2
EOF

# Wait until the SMA starts listening
sma_waitforlisten

localnqn='nqn.2016-06.io.spdk:local0'

# Create a device
device_id=$(create_device $localnqn | jq -r '.handle')

# Check that it's been created
$rpc_py nvmf_get_subsystems $localnqn

# Attach a volume specifying both targets
attach_volume $device_id $t1uuid $t1dscport $t2dscport1

# Check that a connection has been made to discovery services on both targets
[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 2 ]]

$rpc_py bdev_nvme_get_discovery_info | jq -r '.[].trid.trsvcid' | grep $t1dscport
$rpc_py bdev_nvme_get_discovery_info | jq -r '.[].trid.trsvcid' | grep $t2dscport1

# Check that the volume was attached to the device
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces | length') -eq 1 ]]
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces[0].uuid') == "$t1uuid" ]]

# Attach the other volume, this time specify only single target
attach_volume $device_id $t2uuid $t2dscport1

# Check that both volumes are attached to the device
[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 2 ]]
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces | length') -eq 2 ]]
$rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces[].uuid' | grep $t1uuid
$rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces[].uuid' | grep $t2uuid

# Detach the first volume
detach_volume $device_id $t1uuid

# Check that there's a connection to a single target now (because we've only specified a single
# target when connecting the other volume).
[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 1 ]]
$rpc_py bdev_nvme_get_discovery_info | jq -r '.[].trid.trsvcid' | grep $t2dscport1
# Check that the volume was actually removed
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces | length') -eq 1 ]]
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces[0].uuid') == "$t2uuid" ]]

# Detach the other volume
detach_volume $device_id $t2uuid

# And verify it's gone too
[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 0 ]]
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces | length') -eq 0 ]]

# Check that specifying an invalid volume UUID results in an error
NOT attach_volume $device_id $(uuidgen) $t1dscport
[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 0 ]]
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces | length') -eq 0 ]]

# Attach them again, this time both volumes specify both targets
volumes=($t1uuid $t2uuid)
for volume_id in "${volumes[@]}"; do
	attach_volume $device_id $volume_id $t1dscport $t2dscport1
done

[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 2 ]]
$rpc_py bdev_nvme_get_discovery_info | jq -r '.[].trid.trsvcid' | grep $t1dscport
$rpc_py bdev_nvme_get_discovery_info | jq -r '.[].trid.trsvcid' | grep $t2dscport1
$rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces[].uuid' | grep $t1uuid
$rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces[].uuid' | grep $t2uuid

# Detach one and see that both targets are still connected
detach_volume $device_id $t1uuid

[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 2 ]]
$rpc_py bdev_nvme_get_discovery_info | jq -r '.[].trid.trsvcid' | grep $t1dscport
$rpc_py bdev_nvme_get_discovery_info | jq -r '.[].trid.trsvcid' | grep $t2dscport1

# Try to delete the device and verify that it fails if it has volumes attached to it
NOT delete_device $device_id

[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 2 ]]
$rpc_py bdev_nvme_get_discovery_info | jq -r '.[].trid.trsvcid' | grep $t1dscport
$rpc_py bdev_nvme_get_discovery_info | jq -r '.[].trid.trsvcid' | grep $t2dscport1

# After detaching the second volume, it should be possible to delete the device
detach_volume $device_id $t2uuid
delete_device $device_id

[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 0 ]]
NOT $rpc_py nvmf_get_subsystems $localnqn

# Create a device and attach a volume immediately
device_id=$(create_device $localnqn $t1uuid $t1dscport | jq -r '.handle')

# Verify that there's a connection to the target and the volume is attached to the device
[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 1 ]]
$rpc_py bdev_nvme_get_discovery_info | jq -r '.[].trid.trsvcid' | grep $t1dscport
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces | length') -eq 1 ]]
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces[0].uuid') == "$t1uuid" ]]

# Make sure it's also possible to detach it
detach_volume $device_id $t1uuid

[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 0 ]]
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces | length') -eq 0 ]]

# Check that discovery referrals work correctly
attach_volume $device_id $t2uuid $t2dscport1 $t2dscport2

# Check that only a single connection has been made
[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 1 ]]
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces | length') -eq 1 ]]
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces[0].uuid') == "$t2uuid" ]]

# Add the other volume from the same target/subsystem, but use a single discovery endpoint
attach_volume $device_id $t2uuid2 $t2dscport2

# Check that the volume was attached to the subsystem, but no extra connection has been made
[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 1 ]]
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces | length') -eq 2 ]]
$rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces[].uuid' | grep $t2uuid
$rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces[].uuid' | grep $t2uuid2

# Reset the device
detach_volume $device_id $t1uuid
detach_volume $device_id $t2uuid
detach_volume $device_id $t2uuid2
delete_device $device_id
[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 0 ]]

device_id=$(create_device $localnqn | jq -r '.handle')

# Check subsystem NQN verification, start with a valid one
$sma_py <<- EOF
	{
		"method": "AttachVolume",
		"params": {
			"volume": {
				"volume_id": "$(uuid2base64 $t1uuid)",
				"nvmf": {
					"hostnqn": "$hostnqn",
					"subnqn": "$t1nqn",
					"discovery": {
						"discovery_endpoints": [
							{
								"trtype": "tcp",
								"traddr": "127.0.0.1",
								"trsvcid": "$t1dscport"
							}
						]
					}
				}
			},
			"device_handle": "$device_id"
		}
	}
EOF

[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 1 ]]
$rpc_py bdev_nvme_get_discovery_info | jq -r '.[].trid.trsvcid' | grep $t1dscport
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces | length') -eq 1 ]]
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces[0].uuid') == "$t1uuid" ]]

# Then check incorrect subnqn
NOT $sma_py <<- EOF
	{
		"method": "AttachVolume",
		"params": {
			"volume": {
				"volume_id": "$(uuid2base64 $t2uuid)",
				"nvmf": {
					"hostnqn": "$hostnqn",
					"subnqn": "${t2nqn}-invalid",
					"discovery": {
						"discovery_endpoints": [
							{
								"trtype": "tcp",
								"traddr": "127.0.0.1",
								"trsvcid": "$t2dscport1"
							}
						]
					}
				}
			},
			"device_handle": "$device_id"
		}
	}
EOF

# Verify the volume hasn't been attached
[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 1 ]]
$rpc_py bdev_nvme_get_discovery_info | jq -r '.[].trid.trsvcid' | grep $t1dscport
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces | length') -eq 1 ]]
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces[0].uuid') == "$t1uuid" ]]

# Check incorrect hostnqn
NOT $sma_py <<- EOF
	{
		"method": "AttachVolume",
		"params": {
			"volume": {
				"volume_id": "$(uuid2base64 $t2uuid)",
				"nvmf": {
					"hostnqn": "${hostnqn}-invalid",
					"discovery": {
						"discovery_endpoints": [
							{
								"trtype": "tcp",
								"traddr": "127.0.0.1",
								"trsvcid": "$t2dscport1"
							}
						]
					}
				}
			},
			"device_handle": "$device_id"
		}
	}
EOF

# Verify that the volume wasn't attached
[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 1 ]]
$rpc_py bdev_nvme_get_discovery_info | jq -r '.[].trid.trsvcid' | grep $t1dscport
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces | length') -eq 1 ]]
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces[0].uuid') == "$t1uuid" ]]

# Check that the the attach will fail if there's nobody listening on the discovery endpoint
NOT attach_volume $device_id $(uuidgen) $invalid_port
[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 1 ]]
$rpc_py bdev_nvme_get_discovery_info | jq -r '.[].trid.trsvcid' | grep $t1dscport

# Make sure that the discovery service is stopped if a volume is disconnected outside of SMA (e.g.
# by removing it from the target)
$rpc_py -s $t1sock nvmf_subsystem_remove_ns $t1nqn 1
# Give SMA some time to be notified about the change
sleep $((cleanup_period + 1))
[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 0 ]]
$rpc_py -s $t1sock nvmf_subsystem_add_ns $t1nqn $t1uuid

# Do the same, but this time attach two volumes and check that the discovery service is only
# stopped once both volumes are disconnected
attach_volume $device_id $t2uuid $t2dscport1
attach_volume $device_id $t2uuid2 $t2dscport1
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces | length') -eq 2 ]]
[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 1 ]]
$rpc_py -s $t2sock nvmf_subsystem_remove_ns $t2nqn 2
# Give SMA some time to be notified about the change
sleep $((cleanup_period + 1))
# One of the volumes should be gone, but the discovery service should still be running
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces | length') -eq 1 ]]
[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 1 ]]
$rpc_py -s $t2sock nvmf_subsystem_remove_ns $t2nqn 1
# Give SMA some time to be notified about the change
sleep $((cleanup_period + 1))
# Now that both are gone, the discovery service should be stopped too
[[ $($rpc_py nvmf_get_subsystems $localnqn | jq -r '.[].namespaces | length') -eq 0 ]]
[[ $($rpc_py bdev_nvme_get_discovery_info | jq -r '. | length') -eq 0 ]]
$rpc_py -s $t2sock nvmf_subsystem_add_ns $t2nqn $t2uuid
$rpc_py -s $t2sock nvmf_subsystem_add_ns $t2nqn $t2uuid2

delete_device $device_id

cleanup
trap - SIGINT SIGTERM EXIT
