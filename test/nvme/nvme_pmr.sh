#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

function nvme_pmr_persistence() {
	lbas=(4 8 16 32 64 128 256 512 1024 2048 4096)

	for bdf in $(get_nvme_bdfs); do
		for lba in "${lbas[@]}"; do
			$SPDK_EXAMPLE_DIR/pmr_persistence -p ${bdf} -n 1 -r 0 -l $lba -w $lba
		done
	done
}

if [ $(uname) = Linux ]; then
	$rootdir/scripts/setup.sh
fi

run_test "nvme_pmr_persistence" nvme_pmr_persistence
