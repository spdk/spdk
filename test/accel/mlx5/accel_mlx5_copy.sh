#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

function gen_accel_mlx5_malloc_json() {
	accel_qp_size=${1:-256}
	accel_num_requests=${2:-2047}
	accel_driver=${3:-false}

	jq . <<- JSON
		{
		  "subsystems": [
		    {
		      "subsystem": "accel",
		      "config": [
		        {
		          "method": "mlx5_scan_accel_module",
		          "params": {
		            "qp_size": ${accel_qp_size},
		            "num_requests": ${accel_num_requests},
		            "enable_driver": ${accel_driver}
		          }
		        }
		      ]
		    },
		    {
		      "subsystem": "bdev",
		      "config": [
		        {
		          "method": "bdev_malloc_create",
		          "params": {
		            "name": "Malloc0",
		            "num_blocks": 262144,
		            "block_size": 512,
		            "uuid": "e1c24cb1-dd44-4be6-8d67-de92a332013f"
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

# Test copy operation with fragmented payload
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 4096 -O 17 -w verify -t 5 -m 0xf --json <(gen_accel_mlx5_malloc_json) -b "Malloc0" -f -x translate
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 4096 -O 33 -w verify -t 5 -m 0xf --json <(gen_accel_mlx5_malloc_json) -b "Malloc0" -f -x translate
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 524288 -O 79 -w verify -t 5 -m 0xf --json <(gen_accel_mlx5_malloc_json) -b "Malloc0" -f -x translate

# Test lack of resources
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 131072 -O 49 -w verify -t 5 -m 0xf --json <(gen_accel_mlx5_malloc_json 16 2047) -b "Malloc0" -f -x translate
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 131072 -O 49 -w verify -t 5 -m 0xf --json <(gen_accel_mlx5_malloc_json 256 64) -b "Malloc0" -f -x translate
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 131072 -O 49 -w verify -t 5 -m 0xf --json <(gen_accel_mlx5_malloc_json 16 64) -b "Malloc0" -f -x translate

# Test copy operation with fragmented payload and platform driver enabled
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 4096 -O 17 -w verify -t 5 -m 0xf --json <(gen_accel_mlx5_malloc_json 256 2047 true) -b "Malloc0" -f -x translate
