#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

function ftl_kill() {
	rm -f $testdir/.testfile_*
}

if [[ -z "$OCSSD_PCI_DEVICES" -eq 0 ]]; then
	echo "OCSSD device list is empty." 
	echo "This test require that OCSSD_PCI_DEVICES environment variable to be set"
	echo "and point to OCSSD devices PCI BDF. You can specify multiple space
	echo "separated BDFs in this case first one will be used."
	exit 1
fi

set -e

trap "ftl_kill; exit 1" SIGINT SIGTERM EXIT

timing_enter ftl
timing_enter fio

read device _ <<< "$OCSSD_PCI_DEVICES"
run_test suite $testdir/fio.sh $device

timing_exit fio

timing_enter restore
run_test suite $testdir/restore.sh $device $uuid
timing_exit restore

timing_exit ftl

trap - SIGINT SIGTERM EXIT
ftl_kill
