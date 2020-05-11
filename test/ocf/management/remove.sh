#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "${BASH_SOURCE[0]}"))
rootdir=$(readlink -f $curdir/../../..)
source $rootdir/test/common/autotest_common.sh

rpc_py=$rootdir/scripts/rpc.py

rm -f aio*
truncate -s 128M aio0
truncate -s 128M aio1

jq . <<- JSON > "$curdir/config"
	{
	  "subsystems": [
	    {
	      "subsystem": "bdev",
	      "config": [
	        {
	          "method": "bdev_aio_create",
	          "params": {
	            "name": "ai0",
	            "block_size": 512,
	            "filename": "./aio0"
	          }
	        },
	        {
	          "method": "bdev_aio_create",
	          "params": {
	            "name": "aio1",
	            "block_size": 512,
	            "filename": "./aio1"
	          }
	        }
	      ]
	    }
	  ]
	}
JSON

"$SPDK_BIN_DIR/iscsi_tgt" --json "$curdir/config" &
spdk_pid=$!

waitforlisten $spdk_pid

# Create ocf on persistent storage

$rpc_py bdev_ocf_create ocfWT wt aio0 aio1

# Check that ocfWT was created properly

$rpc_py bdev_ocf_get_bdevs | jq -r '.[] .name' | grep -qw ocfWT

# Remove ocfWT, after delete via rpc ocf bdev should not load on next app start

$rpc_py bdev_ocf_delete ocfWT

# Check that ocfWT was deleted properly

! $rpc_py bdev_ocf_get_bdevs | jq -r '.[] .name' | grep -qw ocfWT

trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid

# Check for ocfWT was deleted permanently
"$SPDK_BIN_DIR/iscsi_tgt" --json "$curdir/config" &
spdk_pid=$!

trap 'killprocess $spdk_pid; rm -f aio* $curdir/config ocf_bdevs ocf_bdevs_verify; exit 1' SIGINT SIGTERM EXIT

waitforlisten $spdk_pid

# Check that ocfWT was not loaded on app start

! $rpc_py bdev_ocf_get_bdevs | jq -r '.[] .name' | grep -qw ocfWT

trap - SIGINT SIGTERM EXIT

killprocess $spdk_pid
rm -f aio* $curdir/config ocf_bdevs ocf_bdevs_verify
