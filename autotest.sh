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
	export LCOV_OPTS="
		--rc lcov_branch_coverage=1
		--rc lcov_function_coverage=1
		--rc genhtml_branch_coverage=1
		--rc genhtml_function_coverage=1
		--rc genhtml_legend=1
		--rc geninfo_all_blocks=1
		"
	export LCOV="lcov $LCOV_OPTS"
	export GENHTML="genhtml $LCOV_OPTS"
	# zero out coverage data
	$LCOV -q -c -i -t "Baseline" -d $src -o cov_base.info
fi

# set up huge pages
timing_enter afterboot
./scripts/configure_hugepages.sh 1024
timing_exit afterboot

./scripts/unbind.sh

#####################
# Unit Tests
#####################

timing_enter lib

time test/lib/nvme/nvme.sh
time test/lib/memory/memory.sh
time test/lib/ioat/ioat.sh

timing_exit lib

./scripts/cleanup.sh
./scripts/build_kmod.sh clean

timing_exit autotest
chmod a+r $output_dir/timing.txt

trap - SIGINT SIGTERM EXIT

# catch any stray core files
process_core

if hash lcov; then
	# generate coverage data and combine with baseline
	$LCOV -q -c -d $src -t "$(hostname)" -o cov_test.info
	$LCOV -q -a cov_base.info -a cov_test.info -o cov_total.info
	$LCOV -q -r cov_total.info '/usr/*' -o cov_total.info
	$LCOV -q -r cov_total.info 'test/*' -o cov_total.info
	$GENHTML cov_total.info -t "$(hostname)" -o $out/coverage
	chmod -R a+rX $out/coverage
	rm cov_base.info cov_test.info
	mv cov_total.info $out/cov_total.info
	find . -name "*.gcda" -delete
fi
