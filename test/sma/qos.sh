#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../..")

source "$rootdir/test/common/autotest_common.sh"
source "$testdir/common.sh"

smac="$rootdir/scripts/sma-client.py"

device_nvmf_tcp=3
limit_reserved=$(printf '%u' $((2 ** 64 - 1)))

cleanup() {
	killprocess $tgtpid
	killprocess $smapid
}

create_device() {
	$smac <<- EOF
		{
		  "method": "CreateDevice",
		  "params": {
		    "nvmf_tcp": {
		      "subnqn": "nqn.2016-06.io.spdk:cnode0",
		      "adrfam": "ipv4",
		      "traddr": "127.0.0.1",
		      "trsvcid": "4420"
		    },
		    "volume": {
		      "volume_id": "$(uuid2base64 $1)"
		    }
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

sma_waitforlisten

# Prepare a device with a volume
rpc_cmd bdev_null_create null0 100 4096
uuid=$(rpc_cmd bdev_get_bdevs -b null0 | jq -r '.[].uuid')
device=$(create_device $uuid | jq -r '.handle')

# First check the capabilities
diff <(get_qos_caps $device_nvmf_tcp | jq --sort-keys) <(
	jq --sort-keys <<- EOF
		{
		  "max_volume_caps": {
		    "rw_iops": true,
		    "rd_bandwidth": true,
		    "wr_bandwidth": true,
		    "rw_bandwidth": true
		  }
		}
	EOF
)

# Make sure that invalid device type causes an error
NOT get_qos_caps 1234

# Set a single limit and make sure it's changed (and nothing else was changed)
$smac <<- EOF
	{
	  "method": "SetQos",
	  "params": {
	    "device_handle": "$device",
	    "volume_id": "$(uuid2base64 $uuid)",
	    "maximum": {
	      "rw_iops": 1
	    }
	  }
	}
EOF
diff <(rpc_cmd bdev_get_bdevs -b null0 | jq --sort-keys '.[].assigned_rate_limits') <(
	jq --sort-keys <<- EOF
		{
		  "rw_ios_per_sec": 1000,
		  "rw_mbytes_per_sec": 0,
		  "r_mbytes_per_sec": 0,
		  "w_mbytes_per_sec": 0
		}
	EOF
)

# Change two limits at the same time
$smac <<- EOF
	{
	  "method": "SetQos",
	  "params": {
	    "device_handle": "$device",
	    "volume_id": "$(uuid2base64 $uuid)",
	    "maximum": {
	      "rw_iops": 2,
	      "rd_bandwidth": 8
	    }
	  }
	}
EOF
diff <(rpc_cmd bdev_get_bdevs -b null0 | jq --sort-keys '.[].assigned_rate_limits') <(
	jq --sort-keys <<- EOF
		{
		  "rw_ios_per_sec": 2000,
		  "rw_mbytes_per_sec": 0,
		  "r_mbytes_per_sec": 8,
		  "w_mbytes_per_sec": 0
		}
	EOF
)

# Check that it's possible to preserve existing values by specifying limit=UINT64_MAX
$smac <<- EOF
	{
	  "method": "SetQos",
	  "params": {
	    "device_handle": "$device",
	    "volume_id": "$(uuid2base64 $uuid)",
	    "maximum": {
	      "rw_iops": $limit_reserved,
	      "rd_bandwidth": $limit_reserved,
	      "rw_bandwidth": 6
	    }
	  }
	}
EOF
diff <(rpc_cmd bdev_get_bdevs -b null0 | jq --sort-keys '.[].assigned_rate_limits') <(
	jq --sort-keys <<- EOF
		{
		  "rw_ios_per_sec": 2000,
		  "rw_mbytes_per_sec": 6,
		  "r_mbytes_per_sec": 8,
		  "w_mbytes_per_sec": 0
		}
	EOF
)

# Check that specyfing an unsupported limit results in an error
unsupported_max_limits=(rd_iops wr_iops)

for limit in "${unsupported_max_limits[@]}"; do
	NOT $smac <<- EOF
		{
		  "method": "SetQos",
		  "params": {
		    "device_handle": "$device",
		    "volume_id": "$(uuid2base64 $uuid)",
		    "maximum": {
		      "rw_iops": $limit_reserved,
		      "rd_bandwidth": $limit_reserved,
		      "rw_bandwidth": $limit_reserved,
		      "$limit": 1
		    }
		  }
		}
	EOF
done

# Check non-existing device handle/volume_id
NOT $smac <<- EOF
	{
	  "method": "SetQos",
	  "params": {
	    "device_handle": "${device}-invalid",
	    "volume_id": "$(uuid2base64 $uuid)",
	     "maximum": {
	      "rw_iops": 1
	    }
	  }
	}
EOF

NOT $smac <<- EOF
	{
	  "method": "SetQos",
	  "params": {
	    "device_handle": "$device",
	    "volume_id": "$(uuid2base64 $(uuidgen))",
	     "maximum": {
	      "rw_iops": 1
	    }
	  }
	}
EOF

# Check that it's not possible to set limits without specyfing a volume/device
NOT $smac <<- EOF
	{
	  "method": "SetQos",
	  "params": {
	    "device_handle": "$device",
	     "maximum": {
	      "rw_iops": 1
	    }
	  }
	}
EOF

NOT $smac <<- EOF
	{
	  "method": "SetQos",
	  "params": {
	    "volume_id": "$(uuid2base64 $uuid)",
	     "maximum": {
	      "rw_iops": 1
	    }
	  }
	}
EOF

# Make sure that none of the limits were changed
diff <(rpc_cmd bdev_get_bdevs -b null0 | jq --sort-keys '.[].assigned_rate_limits') <(
	jq --sort-keys <<- EOF
		{
		  "rw_ios_per_sec": 2000,
		  "rw_mbytes_per_sec": 6,
		  "r_mbytes_per_sec": 8,
		  "w_mbytes_per_sec": 0
		}
	EOF
)

trap - SIGINT SIGTERM EXIT
cleanup
