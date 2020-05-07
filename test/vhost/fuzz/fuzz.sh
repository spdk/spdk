#!/usr/bin/env bash
set -e

rootdir=$(readlink -f $(dirname $0))/../../..
source $rootdir/test/common/autotest_common.sh
source "$rootdir/scripts/common.sh"

VHOST_APP+=(-p 0)
FUZZ_RPC_SOCK="/var/tmp/spdk_fuzz.sock"
VHOST_FUZZ_APP+=(-r "$FUZZ_RPC_SOCK" -g --wait-for-rpc)

vhost_rpc_py="$rootdir/scripts/rpc.py"
fuzz_generic_rpc_py="$rootdir/scripts/rpc.py -s $FUZZ_RPC_SOCK"
fuzz_specific_rpc_py="$rootdir/test/app/fuzz/common/fuzz_rpc.py -s $FUZZ_RPC_SOCK"

"${VHOST_APP[@]}" > "$output_dir/vhost_fuzz_tgt_output.txt" 2>&1 &
vhostpid=$!
waitforlisten $vhostpid

trap 'killprocess $vhostpid; exit 1' SIGINT SIGTERM exit

"${VHOST_FUZZ_APP[@]}" -t 10 2> "$output_dir/vhost_fuzz_output1.txt" &
fuzzpid=$!
waitforlisten $fuzzpid $FUZZ_RPC_SOCK

trap 'killprocess $vhostpid; killprocess $fuzzpid; exit 1' SIGINT SIGTERM exit

$vhost_rpc_py bdev_malloc_create -b Malloc0 64 512
$vhost_rpc_py vhost_create_blk_controller Vhost.1 Malloc0

$vhost_rpc_py bdev_malloc_create -b Malloc1 64 512
$vhost_rpc_py vhost_create_scsi_controller naa.VhostScsi0.1
$vhost_rpc_py vhost_scsi_controller_add_target naa.VhostScsi0.1 0 Malloc1

$vhost_rpc_py bdev_malloc_create -b Malloc2 64 512
$vhost_rpc_py vhost_create_scsi_controller naa.VhostScsi0.2
$vhost_rpc_py vhost_scsi_controller_add_target naa.VhostScsi0.2 0 Malloc2

# test the vhost blk controller with valid data buffers.
$fuzz_specific_rpc_py fuzz_vhost_create_dev -s $(pwd)/Vhost.1 -b -v
# test the vhost scsi I/O queue with valid data buffers on a valid lun.
$fuzz_specific_rpc_py fuzz_vhost_create_dev -s $(pwd)/naa.VhostScsi0.1 -l -v
# test the vhost scsi management queue with valid data buffers.
$fuzz_specific_rpc_py fuzz_vhost_create_dev -s $(pwd)/naa.VhostScsi0.2 -v -m
# The test won't actually begin until this option is passed in.
$fuzz_generic_rpc_py framework_start_init

wait $fuzzpid

"${VHOST_FUZZ_APP[@]}" -j "$rootdir/test/app/fuzz/vhost_fuzz/example.json" 2> "$output_dir/vhost_fuzz_output2.txt" &
fuzzpid=$!
waitforlisten $fuzzpid $FUZZ_RPC_SOCK

# re-evaluate fuzzpid
trap 'killprocess $vhostpid; killprocess $fuzzpid; exit 1' SIGINT SIGTERM exit

$fuzz_specific_rpc_py fuzz_vhost_create_dev -s $(pwd)/Vhost.1 -b -v
$fuzz_specific_rpc_py fuzz_vhost_create_dev -s $(pwd)/naa.VhostScsi0.1 -l -v
$fuzz_specific_rpc_py fuzz_vhost_create_dev -s $(pwd)/naa.VhostScsi0.2 -v -m
$fuzz_generic_rpc_py framework_start_init

wait $fuzzpid

trap - SIGINT SIGTERM exit

killprocess $vhostpid
