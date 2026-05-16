#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 NVIDIA Corporation
#  All rights reserved.
#
# Test script for JSON-RPC batching functionality
# Creates null bdevs one at a time, saves config with and without --with-batches,
# verifies batch arrays are correctly emitted and loaded.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

NUM_NULL_BDEVS=50
NULL_BDEV_SIZE=100
NULL_BLOCK_SIZE=512

rpc_py="$rootdir/scripts/rpc.py"

CONFIG_FILE1=$(mktemp)
CONFIG_FILE2=$(mktemp)

function cleanup() {
	rm -f "$CONFIG_FILE1" "$CONFIG_FILE2"
}

trap 'cleanup; exit 1' SIGINT SIGTERM EXIT

"$SPDK_BIN_DIR/spdk_tgt" &
spdk_tgt_pid=$!
waitforlisten $spdk_tgt_pid

# Create null bdevs one at a time
for i in $(seq 1 $NUM_NULL_BDEVS); do
	$rpc_py bdev_null_create Null$i $NULL_BDEV_SIZE $NULL_BLOCK_SIZE
done

# Save config with --with-batches and verify batch arrays are present
$rpc_py save_config --with-batches > "$CONFIG_FILE1"

# Verify bdev_null_create RPCs are emitted as a batch (JSON array)
null_batch_count=$(jq '[.subsystems[] | select(.subsystem == "bdev") | .config[] | select(type == "array") | .[] | select(.method == "bdev_null_create")] | length' "$CONFIG_FILE1")
if [ "$null_batch_count" -ne "$NUM_NULL_BDEVS" ]; then
	echo "ERROR: Expected $NUM_NULL_BDEVS bdev_null_create RPCs in a batch, found $null_batch_count"
	exit 1
fi
echo "PASS: Found $null_batch_count bdev_null_create RPCs in batch"

# Save config with --no-with-batches and verify output is flat (no nested arrays)
$rpc_py save_config --no-with-batches > "$CONFIG_FILE2"

nested_array_count=$(jq '[.subsystems[] | select(.subsystem == "bdev") | .config[] | select(type == "array")] | length' "$CONFIG_FILE2")
if [ "$nested_array_count" -ne 0 ]; then
	echo "ERROR: Expected no nested arrays in non-batch config, found $nested_array_count"
	exit 1
fi
echo "PASS: Non-batch config has no nested arrays"

# Verify the flat config still has all null bdevs
flat_null_count=$(jq '[.subsystems[] | select(.subsystem == "bdev") | .config[] | select(type == "object" and .method == "bdev_null_create")] | length' "$CONFIG_FILE2")
if [ "$flat_null_count" -ne "$NUM_NULL_BDEVS" ]; then
	echo "ERROR: Expected $NUM_NULL_BDEVS bdev_null_create RPCs in flat config, found $flat_null_count"
	exit 1
fi
echo "PASS: Found $flat_null_count bdev_null_create RPCs in flat config"

# Stop the target
killprocess $spdk_tgt_pid

# Start target again with the saved batch config file
"$SPDK_BIN_DIR/spdk_tgt" --json "$CONFIG_FILE1" &
spdk_tgt_pid=$!
waitforlisten $spdk_tgt_pid

# Verify the number of bdevs loaded
bdev_count=$($rpc_py bdev_get_bdevs | jq 'length')
if [ "$bdev_count" -ne "$NUM_NULL_BDEVS" ]; then
	echo "ERROR: Expected $NUM_NULL_BDEVS bdevs after reload, found $bdev_count"
	exit 1
fi
echo "PASS: Loaded $bdev_count bdevs from config"

# Save configuration again
$rpc_py save_config --with-batches > "$CONFIG_FILE2"

# Compare the two config files - they should be identical
if ! diff -q "$CONFIG_FILE1" "$CONFIG_FILE2" > /dev/null; then
	echo "ERROR: Config files differ after reload!"
	diff "$CONFIG_FILE1" "$CONFIG_FILE2"
	exit 1
fi
echo "PASS: Config files are identical after round-trip"

# Cleanup
cleanup
trap - SIGINT SIGTERM EXIT

killprocess $spdk_tgt_pid
