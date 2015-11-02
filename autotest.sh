#!/usr/bin/env bash

rootdir=$(readlink -f $(dirname $0))
source "$rootdir/scripts/autotest_common.sh"

set -xe

if [ $EUID -ne 0 ]; then
	echo "$0 must be run as root"
	exit 1
fi

trap "process_core; $rootdir/scripts/cleanup.sh; exit 1" SIGINT SIGTERM EXIT

timing_enter autotest

src=$(readlink -f $(dirname $0))
out=$PWD
cd $src

if hash lcov; then
	# zero out coverage data
	lcov -q -c -i -t "Baseline" -d $src -o cov_base.info
fi

# set up huge pages
timing_enter afterboot
./scripts/configure_hugepages.sh 3072
timing_exit afterboot

./scripts/unbind_nvme.sh

#####################
# Unit Tests
#####################

timing_enter lib

time test/lib/nvme/nvme.sh
time test/lib/memory/memory.sh

timing_exit lib

./scripts/cleanup.sh

timing_exit autotest
chmod a+r $output_dir/timing.txt

trap - SIGINT SIGTERM EXIT

# catch any stray core files
process_core

if hash lcov; then
	# generate coverage data and combine with baseline
	lcov -q -c -d $src -t "$(hostname)" -o cov_test.info
	lcov -q -a cov_base.info -a cov_test.info -o cov_total.info
	lcov -q -r cov_total.info '/usr/*' -o cov_total.info
	lcov -q -r cov_total.info 'test/*' -o cov_total.info
	genhtml cov_total.info --legend -t "$(hostname)" -o $out/coverage
	chmod -R a+rX $out/coverage
	rm cov_base.info cov_test.info
	mv cov_total.info $out/cov_total.info
	find . -name "*.gcda" -delete
fi
