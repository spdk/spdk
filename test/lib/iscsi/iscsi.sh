#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir="$testdir/../../.."
source $rootdir/scripts/autotest_common.sh

timing_enter iscsi

timing_enter param
$testdir/param/param_ut
timing_exit param

timing_enter target_node
$testdir/target_node/target_node_ut $testdir/target_node/target_node.conf
timing_exit target_node

timing_enter pdu
$testdir/pdu/pdu
timing_exit pdu

timing_exit iscsi
