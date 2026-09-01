#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Silk Technologies, Ltd.
#  All rights reserved.
#
# Regression test for the lib/bdev/part.c read-buffer ownership bug:
# for iov-less reads the bdev layer allocates the data buffer on the
# child bdev_io submitted to the base bdev, and bdev_part_complete_io()
# used to free the child - returning the buffer to the iobuf pool -
# while the iSCSI target was still transmitting the data through the
# part bdev_io. Under concurrent I/O the recycled buffer was refilled
# by other requests before transmission, corrupting the read payloads.
#
# The error vbdev is built on the part framework, so exposing
# EE_Malloc0 over iSCSI (no error injection - only the part I/O
# completion path is exercised) and running verified concurrent I/O
# reproduces the corruption: fio's crc32c verify header embeds each
# block's offset, so a read returning another block's (or recycled)
# buffer content fails verification immediately.
#
# A small socket send buffer keeps data-in PDUs queued in the target
# across many poller iterations; without it an idle loopback target
# copies each payload into the kernel socket in the same iteration
# that completes the read, before the recycled buffer is rewritten,
# and the corruption stays invisible.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/iscsi_tgt/common.sh

iscsitestinit

MALLOC_BDEV_SIZE=256
MALLOC_BLOCK_SIZE=512

fio_py="$rootdir/scripts/fio-wrapper"

timing_enter start_iscsi_tgt

"${ISCSI_APP[@]}" --wait-for-rpc &
pid=$!
echo "Process pid: $pid"

trap 'killprocess $pid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

waitforlisten $pid
$rpc_py iscsi_set_options -o 30 -a 16
# Tiny socket send buffer: forces data-in PDUs to stay queued (still
# referencing their read buffers) while further reads are processed.
$rpc_py sock_impl_set_options -i posix -s 4096
$rpc_py framework_start_init
echo "iscsi_tgt is listening. Running tests..."

timing_exit start_iscsi_tgt

$rpc_py iscsi_create_portal_group $PORTAL_TAG $TARGET_IP:$ISCSI_PORT
$rpc_py iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
# Stack: Malloc0 -> Delay0 -> EE_Delay0 (part-framework vbdev).
# The delay bdev makes base completions asynchronous (like a real
# backend such as bdev_aio); with malloc's inline completions the
# child's buffer is freed, transmitted, and only then reused, so the
# recycling would stay invisible.
$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
$rpc_py bdev_delay_create -b Malloc0 -d Delay0 \
	--avg-read-latency 100 --nine-nine-read-latency 100 \
	--avg-write-latency 100 --nine-nine-write-latency 100
$rpc_py bdev_error_create Delay0
$rpc_py iscsi_create_target_node Target0 Target0_alias 'EE_Delay0:0' $PORTAL_TAG:$INITIATOR_TAG 256 -d
sleep 1

iscsiadm -m discovery -t sendtargets -p $TARGET_IP:$ISCSI_PORT
iscsiadm -m node --login -p $TARGET_IP:$ISCSI_PORT
waitforiscsidevices 1
# Let the initiator-side device probing settle; fio-wrapper writes to
# the device's sysfs queue attributes, which fails with EBUSY while
# the SCSI disk is still being scanned.
sleep 2

trap 'iscsicleanup; killprocess $pid; iscsitestfini; exit 1' SIGINT SIGTERM EXIT

# Lay down verify-tagged data (each block carries a crc32c header
# embedding its offset), then hammer the device with concurrent
# verified reads: any read completed with a recycled buffer returns
# another block's payload and fails verification.
$fio_py -p iscsi -i 131072 -d 32 -n 1 -t write -r 10 -v
$fio_py -p iscsi -i 131072 -d 64 -n 2 -t randread -r 30 -v

trap - SIGINT SIGTERM EXIT

iscsicleanup
killprocess $pid
iscsitestfini
