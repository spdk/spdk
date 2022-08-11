#!/usr/bin/env bash

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../..")

source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/vhost/common.sh"
source "$testdir/common.sh"

function cleanup() {
	killprocess $vhostpid
	killprocess $smapid
	vm_kill_all
}

function create_device() {
	"$rootdir/scripts/sma-client.py" <<- EOF
		{
			"method": "CreateDevice",
			"params": {
			"virtio_blk": {
					"physical_id": "$1",
					"virtual_id": "0"
				},
				"volume": {
					"volume_id": "$(uuid2base64 $2)"
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

trap "cleanup; exit 1" SIGINT SIGTERM EXIT

vm_no=0
timing_enter setup_vm
vm_setup \
	--force=$vm_no \
	--disk-type=virtio \
	--qemu-args="-qmp tcp:localhost:9090,server,nowait -device pci-bridge,chassis_nr=1,id=pci.spdk.0 -device pci-bridge,chassis_nr=2,id=pci.spdk.1" \
	--os="$VM_IMAGE"

vm_run $vm_no
vm_wait_for_boot 300 $vm_no
timing_exit setup_vm

$rootdir/build/bin/vhost -S /var/tmp -m 0x3 &
vhostpid=$!

$rootdir/scripts/sma.py -c <(
	cat <<- EOF
		address: 127.0.0.1
		port: 8080
		devices:
		  - name: 'vhost_blk'
		    params:
		      buses:
		      - name: 'pci.spdk.0'
		        count: 32
		      - name: 'pci.spdk.1'
		        count: 32
		      qmp_addr: 127.0.0.1
		      qmp_port: 9090
		crypto:
		  name: 'bdev_crypto'
		  params:
		    driver: 'crypto_aesni_mb'
	EOF
) &
smapid=$!

# Wait until the SMA starts listening
sma_waitforlisten

# Check that there is no vhost device on guest os
[[ $(vm_exec $vm_no "lsblk | grep -E \"^vd.\" | wc -l") -eq 0 ]]

# Prepare the target
rpc_cmd bdev_null_create null0 100 4096
rpc_cmd bdev_null_create null1 100 4096
uuid=$(rpc_cmd bdev_get_bdevs -b null0 | jq -r '.[].uuid')
uuid2=$(rpc_cmd bdev_get_bdevs -b null1 | jq -r '.[].uuid')

# Create a couple of devices and verify them via RPC
devid0=$(create_device 0 $uuid | jq -r '.handle')
rpc_cmd vhost_get_controllers -n sma-0

devid1=$(create_device 1 $uuid2 | jq -r '.handle')
rpc_cmd vhost_get_controllers -n sma-0
rpc_cmd vhost_get_controllers -n sma-1
[[ "$devid0" != "$devid1" ]]

# Check that there are two controllers (2 created above )
[[ $(rpc_cmd vhost_get_controllers | jq -r '. | length') -eq 2 ]]

# Verify the method is idempotent and sending the same gRPCs won't create new
# devices and will return the same handles
tmp0=$(create_device 0 $uuid | jq -r '.handle')
tmp1=$(create_device 1 $uuid2 | jq -r '.handle')

# Try to duplicate device, this time with different uuid
NOT create_device 1 $uuid | jq -r '.handle'

# Check that there are execly two vhost device on guest os
[[ $(vm_exec $vm_no "lsblk | grep -E \"^vd.\" | wc -l") -eq 2 ]]

[[ $(rpc_cmd vhost_get_controllers | jq -r '. | length') -eq 2 ]]
[[ "$tmp0" == "$devid0" ]]
[[ "$tmp1" == "$devid1" ]]

# Now delete both of them verifying via RPC
delete_device "$devid0"
NOT rpc_cmd vhost_get_controllers -n sma-0
[[ $(rpc_cmd vhost_get_controllers | jq -r '. | length') -eq 1 ]]

delete_device "$devid1"
NOT rpc_cmd vhost_get_controllers -n sma-1
[[ $(rpc_cmd vhost_get_controllers | jq -r '. | length') -eq 0 ]]

# Finally check that removing a non-existing device is also sucessful
delete_device "$devid0"
delete_device "$devid1"

# At the end check if vhost devices are gone
[[ $(vm_exec $vm_no "lsblk | grep -E \"^vd.\" | wc -l") -eq 0 ]]

# Create 62 bdevs, two already exist
for ((i = 2; i < 64; i++)); do
	rpc_cmd bdev_null_create null$i 100 4096
done

devids=()

# Not try to add 64 devices, max for two buses
for ((i = 0; i < 64; i++)); do
	uuid=$(rpc_cmd bdev_get_bdevs -b null$i | jq -r '.[].uuid')
	devids[$i]=$(create_device $i $uuid | jq -r '.handle')
done

[[ $(vm_exec $vm_no "lsblk | grep -E \"^vd.\" | wc -l") -eq 64 ]]

# Cleanup at the end
for ((i = 0; i < 64; i++)); do
	delete_device ${devids[$i]}
done

# And back to none
[[ $(vm_exec $vm_no "lsblk | grep -E \"^vd.\" | wc -l") -eq 0 ]]

key0=1234567890abcdef1234567890abcdef
rpc_cmd bdev_malloc_create -b malloc0 32 4096
uuidc=$(rpc_cmd bdev_get_bdevs -b malloc0 | jq -r '.[].uuid')

#Try to create controller with bdev crypto
devid0=$(
	"$rootdir/scripts/sma-client.py" <<- CREATE | jq -r '.handle'
		{
		  "method": "CreateDevice",
		  "params": {
		    "virtio_blk": {
		      "physical_id": "0",
		      "virtual_id": "0"
		    },
		    "volume": {
		      "volume_id": "$(uuid2base64 $uuidc)",
		      "crypto": {
		        "cipher": "$(get_cipher AES_CBC)",
		        "key": "$(format_key $key0)"
		      }
		    }
		  }
		}
	CREATE
)

[[ $(rpc_cmd vhost_get_controllers | jq -r '. | length') -eq 1 ]]
bdev=$(rpc_cmd vhost_get_controllers | jq -r '.[].backend_specific.block.bdev')

crypto_bdev=$(rpc_cmd bdev_get_bdevs | jq -r '.[] | select(.product_name == "crypto")')
[[ $(jq -r '.driver_specific.crypto.key' <<< "$crypto_bdev") == "$key0" ]]
[[ $(jq -r '.driver_specific.crypto.name' <<< "$crypto_bdev") == "$bdev" ]]

# Delete crypto device and check if it's gone
delete_device $devid0
[[ $(rpc_cmd bdev_get_bdevs | jq -r '.[] | select(.product_name == "crypto")' | jq -r length) -eq 0 ]]

cleanup
trap - SIGINT SIGTERM EXIT
