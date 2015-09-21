#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir="$testdir/../../.."
source $rootdir/scripts/autotest_common.sh

$testdir/vtophys
process_core
