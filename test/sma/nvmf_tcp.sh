#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../..")

source "$rootdir/test/common/autotest_common.sh"
source "$testdir/common.sh"

function cleanup() {
	killprocess $tgtpid
	killprocess $smapid
}

function create_device() {
	"$rootdir/scripts/sma-client.py" <<- EOF
		{
			"method": "CreateDevice",
			"params": {
				"nvmf_tcp": {
					"subnqn": "$1",
					"adrfam": "ipv4",
					"traddr": "127.0.0.1",
					"trsvcid": "4420"
				}
			}
		}
	EOF
}

function delete_device() {
	"$rootdir/scripts/sma-client.py" <<- EOF
		{
			"method": "DeleteDevice",
			"params": {
				"handle": "$1"
			}
		}
	EOF
}

function attach_volume() {
	"$rootdir/scripts/sma-client.py" <<- EOF
		{
			"method": "AttachVolume",
			"params": {
				"device_handle": "$1",
				"volume": {
					"volume_id": "$(uuid2base64 $2)"
				}
			}
		}
	EOF
}

function detach_volume() {
	"$rootdir/scripts/sma-client.py" <<- EOF
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

$rootdir/build/bin/spdk_tgt &
tgtpid=$!

$rootdir/scripts/sma.py -c <(
	cat <<- EOF
		address: 127.0.0.1
		port: 8080
		devices:
		  - name: 'nvmf_tcp'
	EOF
) &
smapid=$!

# Wait until the SMA starts listening
sma_waitforlisten

# Prepare the target
rpc_cmd bdev_null_create null0 100 4096

# Make sure a TCP transport has been created
rpc_cmd nvmf_get_transports --trtype tcp

# Create a couple of devices and verify them via RPC
devid0=$(create_device nqn.2016-06.io.spdk:cnode0 | jq -r '.handle')
rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:cnode0

devid1=$(create_device nqn.2016-06.io.spdk:cnode1 | jq -r '.handle')
rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:cnode0
rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:cnode1
[[ "$devid0" != "$devid1" ]]

# Check that there are three subsystems (2 created above + discovery)
[[ $(rpc_cmd nvmf_get_subsystems | jq -r '. | length') -eq 3 ]]

# Verify the method is idempotent and sending the same gRPCs won't create new
# devices and will return the same handles
tmp0=$(create_device nqn.2016-06.io.spdk:cnode0 | jq -r '.handle')
tmp1=$(create_device nqn.2016-06.io.spdk:cnode1 | jq -r '.handle')

[[ $(rpc_cmd nvmf_get_subsystems | jq -r '. | length') -eq 3 ]]
[[ "$tmp0" == "$devid0" ]]
[[ "$tmp1" == "$devid1" ]]

# Now delete both of them verifying via RPC
delete_device "$devid0"
NOT rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:cnode0
[[ $(rpc_cmd nvmf_get_subsystems | jq -r '. | length') -eq 2 ]]

delete_device "$devid1"
NOT rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:cnode1
[[ $(rpc_cmd nvmf_get_subsystems | jq -r '. | length') -eq 1 ]]

# Finally check that removing a non-existing device is also successful
delete_device "$devid0"
delete_device "$devid1"

# Check volume attach/detach
devid0=$(create_device nqn.2016-06.io.spdk:cnode0 | jq -r '.handle')
devid1=$(create_device nqn.2016-06.io.spdk:cnode1 | jq -r '.handle')
uuid=$(rpc_cmd bdev_get_bdevs -b null0 | jq -r '.[].uuid')

# Attach the volume to a first device
attach_volume "$devid0" "$uuid"
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:cnode0 | jq -r '.[0].namespaces | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:cnode1 | jq -r '.[0].namespaces | length') -eq 0 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:cnode0 | jq -r '.[0].namespaces[0].uuid') == "$uuid" ]]

# Attach the same device again and see that it won't fail
attach_volume "$devid0" "$uuid"
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:cnode0 | jq -r '.[0].namespaces | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:cnode1 | jq -r '.[0].namespaces | length') -eq 0 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:cnode0 | jq -r '.[0].namespaces[0].uuid') == "$uuid" ]]

# Detach it and verify it's removed from the subsystem
detach_volume "$devid0" "$uuid"
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:cnode0 | jq -r '.[0].namespaces | length') -eq 0 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:cnode1 | jq -r '.[0].namespaces | length') -eq 0 ]]

# Detach it again and verify it succeeds
detach_volume "$devid0" "$uuid"

cleanup
trap - SIGINT SIGTERM EXIT
