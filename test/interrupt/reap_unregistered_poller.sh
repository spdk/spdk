#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $testdir/interrupt_common.sh

export PYTHONPATH=$rootdir/examples/interrupt_tgt

# Set reactors with intr_tgt in intr mode
start_intr_tgt

# Record names of native created pollers.
app_thread=$(rpc_cmd thread_get_pollers | jq -r '.threads[0]')
native_pollers=$(jq -r '.active_pollers[].name' <<< $app_thread)
native_pollers+=" "
native_pollers+=$(jq -r '.timed_pollers[].name' <<< $app_thread)

# Create one aio_bdev.
# During the creation, vbdev examine process will get bdev_aio create
# pollers like bdev_aio_group_poll poller, and then unregister it.
setup_bdev_aio

# Record names of remaining pollers.
app_thread=$(rpc_cmd thread_get_pollers | jq -r '.threads[0]')
remaining_pollers=$(jq -r '.active_pollers[].name' <<< $app_thread)
remaining_pollers+=" "
remaining_pollers+=$(jq -r '.timed_pollers[].name' <<< $app_thread)

# Since bdev_aio created pollers were already unregistered, so
# remaining_pollers should be same with native_pollers.
[[ "$remaining_pollers" == "$native_pollers" ]]

trap - SIGINT SIGTERM EXIT
killprocess $intr_tgt_pid
cleanup
