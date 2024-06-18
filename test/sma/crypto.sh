#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../..")

source "$rootdir/test/common/autotest_common.sh"
source "$testdir/common.sh"

rpc_py="$rootdir/scripts/rpc.py"
localnqn=nqn.2016-06.io.spdk:cnode0
tgtnqn=nqn.2016-06.io.spdk:tgt0
key0=1234567890abcdef1234567890abcdef
key1=deadbeefcafebabefeedbeefbabecafe
tgtsock=/var/tmp/spdk.sock2
discovery_port=8009

cleanup() {
	killprocess $smapid
	killprocess $hostpid
	killprocess $tgtpid
}

gen_volume_params() {
	local volume_id=$1 cipher=$2 key=$3 key2=$4 config
	local -a params crypto

	config=$(
		cat <<- PARAMS
			"volume_id": "$(uuid2base64 $volume_id)",
			"nvmf": {
			  "hostnqn": "nqn.2016-06.io.spdk:host0",
			  "discovery": {
			    "discovery_endpoints": [
			      {
			        "trtype": "tcp",
			        "traddr": "127.0.0.1",
			        "trsvcid": "$discovery_port"
			      }
			    ]
			  }
			}
		PARAMS
	)
	params+=("$config")

	local IFS=","
	if [[ -n "$cipher" ]]; then
		crypto+=("\"cipher\": $(get_cipher $cipher)")
		crypto+=("\"key\": \"$(format_key $key)\"")
		if [[ -n "$key2" ]]; then
			crypto+=("\"key2\": \"$(format_key $key2)\"")
		fi

		crypto_config=$(
			cat <<- PARAMS
				"crypto": {
				  ${crypto[*]}
				}
			PARAMS
		)

		params+=("$crypto_config")
	fi

	cat <<- PARAMS
		"volume": {
		  ${params[*]}
		}
	PARAMS
}

create_device() {
	"$rootdir/scripts/sma-client.py" <<- CREATE
		{
		  "method": "CreateDevice",
		  "params": {
		    "nvmf_tcp": {
		      "subnqn": "$localnqn",
		      "adrfam": "ipv4",
		      "traddr": "127.0.0.1",
		      "trsvcid": "4420"
		    }
		    ${1:+, $(gen_volume_params "$@")}
		  }
		}
	CREATE
}

delete_device() {
	"$rootdir/scripts/sma-client.py" <<- DELETE
		{
		  "method": "DeleteDevice",
		  "params": {
		    "handle": "$1"
		  }
		}
	DELETE
}

attach_volume() {
	local device=$1
	shift

	"$rootdir/scripts/sma-client.py" <<- ATTACH
		{
		  "method": "AttachVolume",
		  "params": {
		    "device_handle": "$device",
		    $(gen_volume_params "$@")
		  }
		}
	ATTACH
}

detach_volume() {
	"$rootdir/scripts/sma-client.py" <<- DETACH
		{
		  "method": "DetachVolume",
		  "params": {
		    "device_handle": "$1",
		    "volume_id": "$(uuid2base64 $2)"
		  }
		}
	DETACH
}

verify_crypto_volume() {
	local nqn=$1 uuid=$2 ns ns_bdev

	ns=$(rpc_cmd nvmf_get_subsystems $nqn | jq -r '.[0].namespaces[0]')
	ns_bdev=$(jq -r '.name' <<< "$ns")

	# Make sure that the namespace is a crypto bdev and that there's only a single crypto bdev
	[[ $(rpc_cmd bdev_get_bdevs -b "$ns_bdev" | jq -r '.[0].product_name') == crypto ]]
	[[ $(rpc_cmd bdev_get_bdevs | jq -r '[.[] | select(.product_name == "crypto")] | length') -eq 1 ]]
	# Check that the namespace's UUID/NGUID matches the one requested by the user
	[[ $(jq -r '.uuid' <<< "$ns") == "$uuid" ]]
	[[ $(jq -r '.nguid' <<< "$ns") == "$(uuid2nguid $uuid)" ]]
}

trap "cleanup; exit 1" SIGINT SIGTERM EXIT

"$rootdir/build/bin/spdk_tgt" -m 0x1 --wait-for-rpc &
hostpid=$!

waitforlisten $hostpid

# Configure host with accel crypto parameters
$rpc_py dpdk_cryptodev_scan_accel_module
rpc_cmd dpdk_cryptodev_set_driver -d crypto_aesni_mb
$rpc_py accel_assign_opc -o encrypt -m dpdk_cryptodev
$rpc_py accel_assign_opc -o decrypt -m dpdk_cryptodev
$rpc_py framework_start_init

"$rootdir/build/bin/spdk_tgt" -r "$tgtsock" -m 0x2 &
tgtpid=$!

$rootdir/scripts/sma.py -c <(
	cat <<- CONFIG
		address: 127.0.0.1
		port: 8080
		devices:
		  - name: 'nvmf_tcp'
		crypto:
		  name: 'bdev_crypto'
	CONFIG
) &
smapid=$!

# Wait until the SMA starts listening
sma_waitforlisten

# Prepare the target
uuid=$(uuidgen)
waitforlisten "$tgtpid" "$tgtsock"
$rpc_py -s "$tgtsock" << CONFIG
	bdev_malloc_create -b malloc0 32 4096 -u $uuid
	nvmf_create_transport -t tcp
	nvmf_create_subsystem $tgtnqn -a
	nvmf_subsystem_add_listener discovery -t tcp -a 127.0.0.1 -s $discovery_port
	nvmf_subsystem_add_listener -t tcp -a 127.0.0.1 -s 4421 -f ipv4 $tgtnqn
	nvmf_subsystem_add_ns $tgtnqn malloc0
CONFIG

# Create an empty device first
device=$(create_device | jq -r '.handle')

# First attach a volume without crypto
attach_volume $device $uuid

ns_bdev=$(rpc_cmd nvmf_get_subsystems $localnqn | jq -r '.[0].namespaces[0].name')
[[ $(rpc_cmd bdev_get_bdevs -b "$ns_bdev" | jq -r '.[0].product_name') == 'NVMe disk' ]]
[[ $(rpc_cmd bdev_get_bdevs | jq -r '[.[] | select(.product_name == "crypto")] | length') -eq 0 ]]
[[ $(rpc_cmd nvmf_get_subsystems $localnqn | jq -r '.[0].namespaces[0].uuid') == "$uuid" ]]
[[ $(rpc_cmd nvmf_get_subsystems $localnqn | jq -r '.[0].namespaces[0].nguid') == "$(uuid2nguid $uuid)" ]]

detach_volume $device $uuid

# Now attach a volume with crypto enabled
attach_volume $device $uuid AES_CBC $key0

[[ $(rpc_cmd bdev_nvme_get_discovery_info | jq -r '. | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems $localnqn | jq -r '.[0].namespaces | length') -eq 1 ]]
# Make sure that the namespace exposed in the subsystem is a crypto bdev and is using malloc bdev's UUID
verify_crypto_volume $localnqn $uuid
# Check that it's using correct key
crypto_bdev=$(rpc_cmd bdev_get_bdevs | jq -r '.[] | select(.product_name == "crypto")')
key_name=$(jq -r '.driver_specific.crypto.key_name' <<< "$crypto_bdev")
key_obj=$(rpc_cmd accel_crypto_keys_get -k $key_name)
[[ $(jq -r '.[0].key' <<< "$key_obj") == "$key0" ]]
[[ $(jq -r '.[0].cipher' <<< "$key_obj") == "AES_CBC" ]]

# Attach the same volume again
attach_volume $device $uuid AES_CBC $key0

# Nothing should change
[[ $(rpc_cmd bdev_nvme_get_discovery_info | jq -r '. | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems $localnqn | jq -r '.[0].namespaces | length') -eq 1 ]]
verify_crypto_volume $localnqn $uuid
crypto_bdev2=$(rpc_cmd bdev_get_bdevs | jq -r '.[] | select(.product_name == "crypto")')
[[ $(jq -r '.name' <<< "$crypto_bdev") == $(jq -r '.name' <<< "$crypto_bdev2") ]]
key_name=$(jq -r '.driver_specific.crypto.key_name' <<< "$crypto_bdev2")
key_obj=$(rpc_cmd accel_crypto_keys_get -k $key_name)
[[ $(jq -r '.[0].key' <<< "$key_obj") == "$key0" ]]
[[ $(jq -r '.[0].cipher' <<< "$key_obj") == "AES_CBC" ]]

# Try to do attach it again, but this time use a different crypto algorithm
NOT attach_volume $device $uuid AES_XTS $key0
# Check the same, this time changing the key
NOT attach_volume $device $uuid AES_CBC $key1
# Check the same, this time adding second key
NOT attach_volume $device $uuid AES_CBC $key0 $key1
# Check out-of-range cipher value
NOT attach_volume $device $uuid 8 $key0

# Make sure these failures haven't affected anything
verify_crypto_volume $localnqn $uuid

detach_volume $device $uuid

# Check that if there's something wrong with crypto params, the volume won't get attached and
# everything is cleaned up afterwards
NOT attach_volume $device $uuid 8 $key0
[[ $(rpc_cmd nvmf_get_subsystems $localnqn | jq -r '.[0].namespaces | length') -eq 0 ]]
[[ $(rpc_cmd bdev_nvme_get_discovery_info | jq -r '. | length') -eq 0 ]]
[[ $(rpc_cmd bdev_get_bdevs | jq -r length) -eq 0 ]]

delete_device $device

# Check that it's possible to create a device immediately specyfing a volume with crypto
device=$(create_device $uuid AES_CBC $key0 | jq -r '.handle')
verify_crypto_volume $localnqn $uuid

detach_volume $device $uuid
delete_device $device

# Try to create a device with incorrect volume crypto params, check that it fails and everything
# is cleaned up afterwards
NOT create_device $uuid 8 $key0
[[ $(rpc_cmd bdev_nvme_get_discovery_info | jq -r '. | length') -eq 0 ]]
[[ $(rpc_cmd bdev_get_bdevs | jq -r length) -eq 0 ]]
[[ $(rpc_cmd nvmf_get_subsystems | jq -r "[.[] | select(.nqn == \"$localnqn\")] | length") -eq 0 ]]

# Check that if crypto is disabled, it's not possible to attach a volume with crypto
killprocess $smapid
$rootdir/scripts/sma.py -c <(
	cat <<- CONFIG
		address: 127.0.0.1
		port: 8080
		devices:
		  - name: 'nvmf_tcp'
	CONFIG
) &
smapid=$!

sma_waitforlisten
device=$(create_device | jq -r '.handle')

NOT attach_volume $device $uuid AES_CBC $key0
[[ $(rpc_cmd bdev_nvme_get_discovery_info | jq -r '. | length') -eq 0 ]]
[[ $(rpc_cmd bdev_get_bdevs | jq -r length) -eq 0 ]]

cleanup
trap - SIGINT SIGTERM EXIT
