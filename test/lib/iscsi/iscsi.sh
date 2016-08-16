#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir="$testdir/../../.."
source $rootdir/scripts/autotest_common.sh

timing_enter iscsi

timing_enter param
$testdir/param/param_ut
timing_exit param

timing_exit iscsi
