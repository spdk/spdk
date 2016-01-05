#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir="$testdir/../../.."
source $rootdir/scripts/autotest_common.sh

timing_enter memory

timing_enter vtophys
$testdir/vtophys
timing_exit vtophys

timing_exit memory
