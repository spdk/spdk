#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation.
#  All rights reserved.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/interrupt/common.sh

rpc_py="$rootdir/scripts/rpc.py"

r0_mask=0x1
r1_mask=0x2
r2_mask=0x4

cpu_server_mask=0x07
rpc_server_addr="/var/tmp/spdk.sock"

function start_intr_tgt() {
	local rpc_addr="${1:-$rpc_server_addr}"
	local cpu_mask="${2:-$cpu_server_mask}"

	"$SPDK_EXAMPLE_DIR/interrupt_tgt" -m $cpu_mask -r $rpc_addr -E -g &
	intr_tgt_pid=$!
	trap 'killprocess "$intr_tgt_pid"; cleanup; exit 1' SIGINT SIGTERM EXIT
	waitforlisten "$intr_tgt_pid" $rpc_addr
}
