#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

if [ "$TEST_TRANSPORT" != "rdma" ]; then
	exit 0
fi

MALLOC_BDEV_SIZE=256
MALLOC_BLOCK_SIZE=512
subsystem="0"

function gen_malloc_json() {
	jq . <<- JSON
		{
		  "subsystems": [
		    {
		      "subsystem": "bdev",
		      "config": [
		        {
		          "method": "bdev_malloc_create",
		          "params": {
		            "name": "Malloc0",
		            "num_blocks": 131072,
		            "block_size": 512,
		            "uuid": "e1c24cb1-dd44-4be6-8d67-de92a332013f",
		            "optimal_io_boundary": 2
		          }
		        },
		        {
		          "method": "bdev_wait_for_examine"
		        }
		      ]
		    }
		  ]
		}
	JSON
}

function gen_lvol_nvme_json() {
	local subsystem=$1

	jq . <<- JSON
		{
		  "subsystems": [
		    {
		      "subsystem": "bdev",
		      "config": [
		        {
		          "method": "bdev_nvme_attach_controller",
		          "params": {
		            "name": "Nvme${subsystem}",
		            "trtype": "$TEST_TRANSPORT",
		            "adrfam": "IPv4",
		            "traddr": "$NVMF_FIRST_TARGET_IP",
		            "trsvcid": "$NVMF_PORT",
		            "subnqn": "nqn.2016-06.io.spdk:cnode${subsystem}"
		          }
		        },
		        {
		          "method": "bdev_lvol_create_lvstore",
		          "params": {
		            "bdev_name": "Nvme${subsystem}n1",
		            "lvs_name": "lvs${subsystem}"
		          }
		        },
		        {
		          "method": "bdev_lvol_create",
		          "params": {
		            "lvol_name": "lvol${subsystem}",
		            "size": 134217728,
		            "thin_provision": true,
		            "lvs_name": "lvs${subsystem}"
		          }
		        },
		        {
		          "method": "bdev_wait_for_examine"
		        }
		      ]
		    }
		  ]
		}
	JSON
}

nvmftestinit
nvmfappstart -m 0x3

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc0
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode$subsystem -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode$subsystem Malloc0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode$subsystem -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# test memory translation
# test_dma doesn't use RPC, but we change the sock path since nvmf target is already using the default RPC sock
"$rootdir/test/dma/test_dma/test_dma" -q 16 -o 4096 -w randrw -M 70 -t 5 -m 0xc --json <(gen_nvmf_target_json $subsystem) -b "Nvme${subsystem}n1" -f -x translate -r /var/tmp/dma.sock

# test data pull/push with split against local malloc
"$rootdir/test/dma/test_dma/test_dma" -q 16 -o 4096 -w randrw -M 70 -t 5 -m 0xc --json <(gen_malloc_json) -b "Malloc0" -x pull_push -r /var/tmp/dma.sock

# test memzero with logical volumes. All clusters are unallocated, read should trigger memzero
"$rootdir/test/dma/test_dma/test_dma" -q 16 -o 4096 -w randread -M 70 -t 5 -m 0xc --json <(gen_lvol_nvme_json $subsystem) -b "lvs${subsystem}/lvol${subsystem}" -f -x memzero -r /var/tmp/dma.sock

# clear blob metadata
$rootdir/build/examples/perf -q 16 -o 4096 -w write -t 1 -r "trtype:$TEST_TRANSPORT adrfam:IPV4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT"

# test memory translation with logical volumes
"$rootdir/test/dma/test_dma/test_dma" -q 16 -o 4096 -w randrw -M 70 -t 5 -m 0xc --json <(gen_lvol_nvme_json $subsystem) -b "lvs${subsystem}/lvol${subsystem}" -f -x translate -r /var/tmp/dma.sock

trap - SIGINT SIGTERM EXIT

nvmftestfini
