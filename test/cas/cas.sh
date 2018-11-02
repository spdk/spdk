#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)

source $rootdir/test/common/autotest_common.sh

timing_enter cas

for dir in $(ls -d1 $testdir/*/); do
	timing_enter $dir
	for file in $(ls -1 ${dir}*); do
		if [[ -x "$file" ]]; then
			timing_enter $file
			run_test suite $file
			timing_exit $file
		fi
	done
	timing_exit $dir
done

timing_exit cas
