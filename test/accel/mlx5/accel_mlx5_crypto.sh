#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

allowed_devices=${1:-"mlx5_0"}

function gen_accel_mlx5_crypto_json() {
	crypto_split_blocks=${1:-0}
	accel_qp_size=${2:-256}
	accel_num_requests=${3:-2047}
	accel_driver=${4:-false}

	jq . <<- JSON
		{
		  "subsystems": [
		    {
		      "subsystem": "accel",
		      "config": [
		        {
		          "method": "mlx5_scan_accel_module",
		          "params": {
		            "allowed_devs": "${allowed_devices}",
		            "qp_size": ${accel_qp_size},
		            "num_requests": ${accel_num_requests},
		            "crypto_split_blocks": ${crypto_split_blocks},
		            "enable_driver": ${accel_driver}
		          }
		        },
		        {
		          "method": "accel_crypto_key_create",
		          "params": {
		            "name": "test_dek",
		            "cipher": "AES_XTS",
		            "key": "00112233445566778899001122334455",
		            "key2": "11223344556677889900112233445500"
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
		            "num_blocks": 131072,
		            "block_size": 512,
		            "uuid": "e1c24cb1-dd44-4be6-8d67-de92a332013f"
		          }
		        },
		        {
		          "method": "bdev_crypto_create",
		          "params": {
		            "base_bdev_name": "Malloc0",
		            "name": "Crypto0",
		            "key_name": "test_dek"
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

# Test crypto_split_blocks
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 16384 -w verify -t 5 -m 0xc --json <(gen_accel_mlx5_crypto_json 8) -b "Crypto0" -f -x translate

# Test fragmented crypto operation
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 4096 -O 17 -w verify -t 5 -m 0xc --json <(gen_accel_mlx5_crypto_json) -b "Crypto0" -f -x translate
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 4096 -O 33 -w verify -t 5 -m 0xc --json <(gen_accel_mlx5_crypto_json) -b "Crypto0" -f -x translate
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 131072 -O 49 -w verify -t 5 -m 0xc --json <(gen_accel_mlx5_crypto_json) -b "Crypto0" -f -x translate
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 131072 -O 49 -w verify -t 5 -m 0xc --json <(gen_accel_mlx5_crypto_json 5) -b "Crypto0" -f -x translate

# Test fragmented crypto operation with platform driver enabled
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 131072 -O 49 -w verify -t 5 -m 0xc --json <(gen_accel_mlx5_crypto_json 0 256 2047 true) -b "Crypto0" -f -x translate

# Test lack of resources
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 131072 -O 49 -w verify -t 5 -m 0xc --json <(gen_accel_mlx5_crypto_json 0 16 2047) -b "Crypto0" -f -x translate
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 131072 -O 49 -w verify -t 5 -m 0xc --json <(gen_accel_mlx5_crypto_json 0 256 32) -b "Crypto0" -f -x translate
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 131072 -O 49 -w verify -t 5 -m 0xc --json <(gen_accel_mlx5_crypto_json 0 16 32) -b "Crypto0" -f -x translate

# Test different modes, qdepth and IO size values
for mode in randread randwrite randrw; do
	for qdepth in 64 256; do
		for io_size in 512 4096 65536 131072; do
			echo "test: mode $mode, qdepth $qdepth, io_size $io_size"
			"$rootdir/test/dma/test_dma/test_dma" -q $qdepth -o $io_size -w $mode -M 50 -t 5 -m 0xc --json <(gen_accel_mlx5_crypto_json) -b "Crypto0" -f -x translate
		done
	done
done

# Test qp recovery
"$rootdir/test/dma/test_dma/test_dma" -q 64 -o 4096 -w randrw -M 50 -t 5 -m 0xc --json <(gen_accel_mlx5_crypto_json 8) -b "Crypto0" -f -x translate -Y 1000000

trap - SIGINT SIGTERM EXIT
