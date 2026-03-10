#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

CLEANUP_TIMEOUT=10

function get_unassociated_qpairs() {
	$rpc_py nvmf_get_stats \
		| jq '[.poll_groups[].current_unassociated_qpairs] | add'
}

unassociated_count=0

function check_qpairs_cleaned() {
	unassociated_count=$(get_unassociated_qpairs)
	[ "$unassociated_count" -eq 0 ]
}

nvmftestinit
nvmfappstart

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS
$rpc_py nvmf_subsystem_add_listener nqn.2014-08.org.nvmexpress.discovery \
	-t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

sleep 1

# Simulate a port scanner: connect, send garbage data exceeding the
# PDU common header size (8 bytes), and close the socket immediately.
# Using socat -u makes this write-only and exits after stdin is exhausted,
# leaving buffered data in SPDK's recv queue while the peer has already gone.
head -c 128 /dev/urandom | socat -u - "TCP:$NVMF_FIRST_TARGET_IP:$NVMF_PORT"
sleep 1

if ! waitforcondition 'check_qpairs_cleaned' $CLEANUP_TIMEOUT; then
	echo "ERROR: $unassociated_count unassociated qpairs remaining after ${CLEANUP_TIMEOUT}s"
	false
fi

trap - SIGINT SIGTERM EXIT
nvmftestfini
