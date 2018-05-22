#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

timing_enter lnvm

if [ `uname` = Linux ]; then
	start_stub "-s 2048 -i 0 -m 0xF"
	trap "kill_stub; exit 1" SIGINT SIGTERM EXIT
fi

timing_enter identify
for bdf in $(iter_pci_class_code 01 08 02); do
	$rootdir/examples/nvme/identify/identify -r "trtype:PCIe traddr:${bdf}" -i 0
done
timing_exit identify

if [ `uname` = Linux ]; then
	trap - SIGINT SIGTERM EXIT
	kill_stub
fi

timing_exit lnvm
