#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2024, NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

function gen_accel_mlx5_json() {
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
		    }
		  ]
		}
	JSON
}

accelperf=$rootdir/build/examples/accel_perf

$accelperf -c <(gen_accel_mlx5_json) -w crc32c -t 5 -m 0xf -y -C 1 -o 4096 -q 64
$accelperf -c <(gen_accel_mlx5_json) -w crc32c -t 5 -m 0xf -y -C 33 -o 4096 -q 64
$accelperf -c <(gen_accel_mlx5_json) -w crc32c -t 5 -m 0xf -y -C 33 -o 131072 -q 128
# accel perf consumes to much memory in this test, lower qd and number of cores
$accelperf -c <(gen_accel_mlx5_json) -w crc32c -t 5 -m 0x3 -y -C 77 -o 524288 -q 64

$accelperf -c <(gen_accel_mlx5_json) -w copy_crc32c -t 5 -m 0xf -y -C 1 -o 4096 -q 64
$accelperf -c <(gen_accel_mlx5_json) -w copy_crc32c -t 5 -m 0xf -y -C 33 -o 4096 -q 64
$accelperf -c <(gen_accel_mlx5_json) -w copy_crc32c -t 5 -m 0xf -y -C 33 -o 131072 -q 128
# accel perf consumes to much memory in this test, lower qd and number of cores
$accelperf -c <(gen_accel_mlx5_json) -w copy_crc32c -t 5 -m 0x3 -y -C 77 -o 524288 -q 64

# Test with small amount of resources
$accelperf -c <(gen_accel_mlx5_json 16 2047) -w crc32c -t 5 -m 0x3 -y -C 17 -o 131072 -q 128
$accelperf -c <(gen_accel_mlx5_json 256 32) -w crc32c -t 5 -m 0x3 -y -C 17 -o 131072 -q 128
$accelperf -c <(gen_accel_mlx5_json 16 32) -w crc32c -t 5 -m 0x3 -y -C 17 -o 131072 -q 128

$accelperf -c <(gen_accel_mlx5_json 16 2047) -w copy_crc32c -t 5 -m 0x3 -y -C 17 -o 131072 -q 128
$accelperf -c <(gen_accel_mlx5_json 256 32) -w copy_crc32c -t 5 -m 0x3 -y -C 17 -o 131072 -q 128
$accelperf -c <(gen_accel_mlx5_json 16 32) -w copy_crc32c -t 5 -m 0x3 -y -C 17 -o 131072 -q 128

# Test copy operation with fragmented payload and platform driver enabled
$accelperf -c <(gen_accel_mlx5_json 256 2047 true) -w crc32c -t 5 -m 0x3 -y -C 17 -o 131072 -q 128
$accelperf -c <(gen_accel_mlx5_json 256 2047 true) -w copy_crc32c -t 5 -m 0x3 -y -C 17 -o 131072 -q 128

if [ "$TEST_TRANSPORT" != "tcp" ]; then
	exit 0
fi

MALLOC_BDEV_SIZE=256
MALLOC_BLOCK_SIZE=512

function gen_accel_mlx5_crc_json() {

	jq . <<- JSON
		{
		  "subsystems": [
		    {
		      "subsystem": "accel",
		      "config": [
		        {
		          "method": "mlx5_scan_accel_module",
		          "params": {
		          }
		        }
		      ]
		    },
		    {
		      "subsystem": "bdev",
		      "config": [
		        {
		          "method": "bdev_nvme_attach_controller",
		          "params": {
		            "name": "Nvme0",
		            "trtype": "$TEST_TRANSPORT",
		            "adrfam": "IPv4",
		            "traddr": "$NVMF_FIRST_TARGET_IP",
		            "trsvcid": "$NVMF_PORT",
		            "subnqn": "nqn.2016-06.io.spdk:cnode0",
		            "ddgst": true
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
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode0 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 Malloc0
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# test crc32c with TCP data digest
bdevperf=$rootdir/build/examples/bdevperf
$bdevperf --json <(gen_accel_mlx5_crc_json) -q 128 -o 4096 -t 5 -w randrw -M 50 -m 0xc -r /var/tmp/bdev.sock
$bdevperf --json <(gen_accel_mlx5_crc_json) -q 128 -o 131072 -t 5 -w randrw -M 50 -m 0xc -r /var/tmp/bdev.sock

trap - SIGINT SIGTERM EXIT

nvmftestfini
