#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../..")

source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/vfio_user/common.sh"
source "$testdir/common.sh"

function create_device() {
	local pfid=${1:-1}
	local vfid=${2:-0}

	"$rootdir/scripts/sma-client.py" <<- CREATE
		{
		  "method": "CreateDevice",
		  "params": {
		    "nvme": {
		      "physical_id": "$pfid",
		      "virtual_id": "$vfid"
		    }
		  }
		}
	CREATE
}

function delete_device() {
	"$rootdir/scripts/sma-client.py" <<- DELETE
		{
		  "method": "DeleteDevice",
		  "params": {
		    "handle": "$1"
		  }
		}
	DELETE
}

function attach_volume() {
	"$rootdir/scripts/sma-client.py" <<- ATTACH
		{
		  "method": "AttachVolume",
		  "params": {
		    "device_handle": "$1",
		    "volume": {
		      "volume_id": "$(uuid2base64 $2)"
		    }
		  }
		}
	ATTACH
}

function detach_volume() {
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

function vm_count_nvme() {
	vm_exec $1 "grep -l SPDK /sys/class/nvme/*/model" | wc -l
}

function vm_check_subsys_volume() {
	local vm_id=$1
	local nqn=$2
	local uuid=$3

	nvme="$(vm_exec $vm_id "grep -l $nqn /sys/class/nvme/*/subsysnqn" | awk -F/ '{print $5}')"
	if [[ -z "$nvme" ]]; then
		error "FAILED no NVMe on vm=$vm_id with nqn=$nqn"
		return 1
	fi

	tmpuuid="$(vm_exec $vm_id "grep -l $uuid /sys/class/nvme/$nvme/nvme*/uuid")"
	if [[ -z "$tmpuuid" ]]; then
		return 1
	fi
}

function vm_check_subsys_nqn() {
	sleep 1
	nqn=$(vm_exec $1 "grep -l $2 /sys/class/nvme/*/subsysnqn")
	if [[ -z "$nqn" ]]; then
		error "FAILED no NVMe on vm=$1 with nqn=$2"
		return 1
	fi
}

function cleanup() {
	vm_kill_all
	killprocess $tgtpid
	killprocess $smapid
	if [ -e "${VFO_ROOT_PATH}" ]; then rm -rf "${VFO_ROOT_PATH}"; fi
}

trap "cleanup; exit 1" SIGINT SIGTERM EXIT

# SSH VM Password
VM_PASSWORD=root
vm_no=0

VFO_ROOT_PATH="/tmp/sma/vfio-user/qemu"

if [ -e "${VFO_ROOT_PATH}" ]; then rm -rf "${VFO_ROOT_PATH}"; fi
mkdir -p "${VFO_ROOT_PATH}"

# Cleanup old VM:
used_vms=$vm_no
vm_kill_all

vm_setup --os="$VM_IMAGE" --disk-type=virtio --force=$vm_no --qemu-args="-qmp tcp:localhost:10005,server,nowait \
-device pci-bridge,chassis_nr=1,id=pci.spdk.0 \
-device pci-bridge,chassis_nr=2,id=pci.spdk.1"

# Run pre-configured VM and wait for them to start
vm_run $vm_no
vm_wait_for_boot 300 $vm_no

# Start SPDK
$rootdir/build/bin/spdk_tgt --wait-for-rpc &
tgtpid=$!
waitforlisten $tgtpid

# Configure accel crypto module & operations
rpc_cmd dpdk_cryptodev_scan_accel_module
rpc_cmd dpdk_cryptodev_set_driver -d crypto_aesni_mb
rpc_cmd accel_assign_opc -o encrypt -m dpdk_cryptodev
rpc_cmd accel_assign_opc -o decrypt -m dpdk_cryptodev
rpc_cmd framework_start_init

# Prepare the target
rpc_cmd bdev_null_create null0 100 4096
rpc_cmd bdev_null_create null1 100 4096

# Start SMA server
$rootdir/scripts/sma.py -c <(
	cat <<- EOF
		devices:
		  - name: 'vfiouser'
		    params:
		      buses:
		        - name: 'pci.spdk.0'
		          count: 32
		        - name: 'pci.spdk.1'
		          count: 32
		      qmp_addr: 127.0.0.1
		      qmp_port: 10005
		crypto:
		  name: 'bdev_crypto'
	EOF
) &
smapid=$!

# Wait until the SMA starts listening
sma_waitforlisten

# Make sure a TCP transport has been created
rpc_cmd nvmf_get_transports --trtype VFIOUSER

# Make sure no nvme subsystems are present
[[ $(vm_exec ${vm_no} nvme list-subsys -o json | jq -r '.Subsystems | length') -eq 0 ]]

# Create a couple of devices and verify them via RPC and SSH
device0=$(create_device 0 0 | jq -r '.handle')
rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0
vm_check_subsys_nqn $vm_no nqn.2016-06.io.spdk:vfiouser-0

# Check that there are two subsystems (1 created above + discovery)
[[ $(rpc_cmd nvmf_get_subsystems | jq -r '. | length') -eq 2 ]]

device1=$(create_device 1 0 | jq -r '.handle')
rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0
rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1
[[ "$device0" != "$device1" ]]
vm_check_subsys_nqn $vm_no nqn.2016-06.io.spdk:vfiouser-1

# Check that there are three subsystems (2 created above + discovery)
[[ $(rpc_cmd nvmf_get_subsystems | jq -r '. | length') -eq 3 ]]

# Verify the method is idempotent and sending the same gRPCs won't create new
# devices and will return the same IDs
tmp0=$(create_device 0 0 | jq -r '.handle')
tmp1=$(create_device 1 0 | jq -r '.handle')

[[ $(vm_count_nvme ${vm_no}) -eq 2 ]]

[[ $(rpc_cmd nvmf_get_subsystems | jq -r '. | length') -eq 3 ]]
[[ "$tmp0" == "$device0" ]]
[[ "$tmp1" == "$device1" ]]

# Now remove them verifying via RPC
delete_device "$device0"
NOT rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0
rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1
[[ $(rpc_cmd nvmf_get_subsystems | jq -r '. | length') -eq 2 ]]
[[ $(vm_count_nvme ${vm_no}) -eq 1 ]]

delete_device "$device1"
NOT rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0
NOT rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1
[[ $(rpc_cmd nvmf_get_subsystems | jq -r '. | length') -eq 1 ]]
[[ $(vm_count_nvme ${vm_no}) -eq 0 ]]

# Finally check that removing a non-existing device is also successful
delete_device "$device0"
delete_device "$device1"

# Check volume attach/detach
device0=$(create_device 0 0 | jq -r '.handle')
device1=$(create_device 1 0 | jq -r '.handle')
uuid0=$(rpc_cmd bdev_get_bdevs -b null0 | jq -r '.[].uuid')
uuid1=$(rpc_cmd bdev_get_bdevs -b null1 | jq -r '.[].uuid')

# Attach the first volume to a first subsystem
attach_volume "$device0" "$uuid0"
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1 | jq -r '.[0].namespaces | length') -eq 0 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces[0].uuid') == "$uuid0" ]]
vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-0 $uuid0

attach_volume "$device1" "$uuid1"
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1 | jq -r '.[0].namespaces | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces[0].uuid') == "$uuid0" ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1 | jq -r '.[0].namespaces[0].uuid') == "$uuid1" ]]
vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-1 $uuid1

# Attach the same device again and see that it won't fail
attach_volume "$device0" "$uuid0"
attach_volume "$device1" "$uuid1"
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1 | jq -r '.[0].namespaces | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces[0].uuid') == "$uuid0" ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1 | jq -r '.[0].namespaces[0].uuid') == "$uuid1" ]]
vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-0 $uuid0
NOT vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-0 $uuid1
vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-1 $uuid1
NOT vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-1 $uuid0

# Cross detach volumes and verify they not fail and have not been removed from the subsystems
detach_volume "$device0" "$uuid1"
detach_volume "$device1" "$uuid0"
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1 | jq -r '.[0].namespaces | length') -eq 1 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces[0].uuid') == "$uuid0" ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1 | jq -r '.[0].namespaces[0].uuid') == "$uuid1" ]]
vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-0 $uuid0
NOT vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-0 $uuid1
vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-1 $uuid1
NOT vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-1 $uuid0

# Detach volumes and verify they have been removed from the subsystems
detach_volume "$device0" "$uuid0"
detach_volume "$device1" "$uuid1"
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces | length') -eq 0 ]]
[[ $(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-1 | jq -r '.[0].namespaces | length') -eq 0 ]]
NOT vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-0 $uuid0
NOT vm_check_subsys_volume $vm_no nqn.2016-06.io.spdk:vfiouser-1 $uuid1

# Detach volumes once again and verify they will not fail
detach_volume "$device0" "$uuid0"
detach_volume "$device1" "$uuid1"
detach_volume "$device0" "$uuid1"
detach_volume "$device1" "$uuid0"

delete_device "$device0"
delete_device "$device1"

# Create device with allocation on second bus
device3=$(create_device 42 0 | jq -r '.handle')
vm_check_subsys_nqn $vm_no nqn.2016-06.io.spdk:vfiouser-42

# Verify that device can be found on second bus and properly deleted
delete_device "$device3"
NOT vm_check_subsys_nqn $vm_no nqn.2016-06.io.spdk:vfiouser-42

key0=1234567890abcdef1234567890abcdef
device0=$(create_device 0 0 | jq -r '.handle')
uuid0=$(rpc_cmd bdev_get_bdevs -b null0 | jq -r '.[].uuid')

# Now check vfio-user attach with bdev crypto
"$rootdir/scripts/sma-client.py" <<- ATTACH
	{
	  "method": "AttachVolume",
	  "params": {
	    "device_handle": "$device0",
	    "volume": {
	      "volume_id": "$(uuid2base64 $uuid0)",
	      "crypto": {
	        "cipher": "$(get_cipher AES_CBC)",
	        "key": "$(format_key $key0)"
	      }
	    }
	  }
	}
ATTACH

# Make sure that the namespace exposed in the subsystem is a crypto bdev
ns_bdev=$(rpc_cmd nvmf_get_subsystems nqn.2016-06.io.spdk:vfiouser-0 | jq -r '.[0].namespaces[0].name')
[[ $(rpc_cmd bdev_get_bdevs -b "$ns_bdev" | jq -r '.[0].product_name') == "crypto" ]]
crypto_bdev=$(rpc_cmd bdev_get_bdevs -b "$ns_bdev" | jq -r '.[] | select(.product_name == "crypto")')
[[ $(rpc_cmd bdev_get_bdevs | jq -r '[.[] | select(.product_name == "crypto")] | length') -eq 1 ]]

key_name=$(jq -r '.driver_specific.crypto.key_name' <<< "$crypto_bdev")
key_obj=$(rpc_cmd accel_crypto_keys_get -k $key_name)
[[ $(jq -r '.[0].key' <<< "$key_obj") == "$key0" ]]
[[ $(jq -r '.[0].cipher' <<< "$key_obj") == "AES_CBC" ]]

detach_volume "$device0" "$uuid0"
delete_device "$device0"
[[ $(rpc_cmd bdev_get_bdevs | jq -r '.[] | select(.product_name == "crypto")' | jq -r length) -eq 0 ]]

# Test qos
device_vfio_user=1
device0=$(create_device 0 0 | jq -r '.handle')
attach_volume "$device0" "$uuid0"

# First check the capabilities
diff <(get_qos_caps $device_vfio_user | jq --sort-keys) <(
	jq --sort-keys <<- CAPS
		{
		  "max_volume_caps": {
		    "rw_iops": true,
		    "rd_bandwidth": true,
		    "wr_bandwidth": true,
		    "rw_bandwidth": true
		  }
		}
	CAPS
)

"$rootdir/scripts/sma-client.py" <<- EOF
	{
	  "method": "SetQos",
	  "params": {
	    "device_handle": "$device0",
	    "volume_id": "$(uuid2base64 $uuid0)",
	    "maximum": {
	      "rd_iops": 0,
	      "wr_iops": 0,
	      "rw_iops": 3,
	      "rd_bandwidth": 4,
	      "wr_bandwidth": 5,
	      "rw_bandwidth": 6
	    }
	  }
	}
EOF

# Make sure that limits were changed
diff <(rpc_cmd bdev_get_bdevs -b null0 | jq --sort-keys '.[].assigned_rate_limits') <(
	jq --sort-keys <<- EOF
		{
		  "rw_ios_per_sec": 3000,
		  "rw_mbytes_per_sec": 6,
		  "r_mbytes_per_sec": 4,
		  "w_mbytes_per_sec": 5
		}
	EOF
)

detach_volume "$device0" "$uuid0"
delete_device "$device0"

cleanup
trap - SIGINT SIGTERM EXIT
