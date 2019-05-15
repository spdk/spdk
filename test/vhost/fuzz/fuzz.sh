#!/usr/bin/env bash
set -e

rootdir=$(readlink -f $(dirname $0))/../../..
source $rootdir/test/common/autotest_common.sh
source "$rootdir/scripts/common.sh"

rpc_py="$rootdir/scripts/rpc.py"

VHOST_APP="$rootdir/app/vhost/vhost -p 0"
FUZZ_APP="$rootdir/test/app/fuzz/vhost_fuzz/vhost_fuzz"

timing_enter fuzz_test

$VHOST_APP >$output_dir/vhost_fuzz_tgt_output.txt 2>&1 &
vhostpid=$!
waitforlisten $vhostpid

trap "killprocess $vhostpid; exit 1" SIGINT SIGTERM exit

$rpc_py construct_malloc_bdev -b Malloc0 64 512
$rpc_py construct_vhost_blk_controller Vhost.1 Malloc0

$rpc_py construct_malloc_bdev -b Malloc1 64 512
$rpc_py construct_vhost_scsi_controller naa.VhostScsi0.1
$rpc_py add_vhost_scsi_lun naa.VhostScsi0.1 0 Malloc1

# test the vhost blk controller with valid data buffer.
$FUZZ_APP -b -s `pwd`/Vhost.1 -t 10 -a

# test the vhost scsi I/O queue with valid data buffer on a valid lun.
$FUZZ_APP -s `pwd`/naa.VhostScsi0.1 -t 10 -l -a

# test the vhost scsi control queue with valid data buffer.
$FUZZ_APP -s `pwd`/naa.VhostScsi0.1 -t 10 -a -m

trap - SIGINT SIGTERM exit

killprocess $vhostpid
timing_exit fuzz_test
