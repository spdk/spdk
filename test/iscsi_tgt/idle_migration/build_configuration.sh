#!/usr/bin/env bash

set -xe
rootdir=$(readlink -f $(dirname $0))/../../..

rpc_py=$rootdir/scripts/rpc.py

"$rpc_py" add_initiator_group 1 "ALL" "127.0.0.1/32"
"$rpc_py" add_portal_group 1 '127.0.0.1:3260'

for i in $(seq 0 15); do
    "$rpc_py" construct_malloc_bdev 32 512
    "$rpc_py" construct_target_node "Target$i" "Target_alias$i" "Malloc$i:0" "1:1" 64 1 0 0 0
done
