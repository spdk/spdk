#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))
source "$rootdir/test/common/autotest_common.sh"

out=$PWD

umask 022

cd $rootdir

date -u
git describe --tags

./configure $config_params

# Print some test system info out for the log
echo "** START ** Info for Hostname: $HOSTNAME"
uname -a
$MAKE cc_version
$MAKE cxx_version
echo "** END ** Info for Hostname: $HOSTNAME"

timing_enter autobuild

timing_enter check_format
if [ $SPDK_RUN_CHECK_FORMAT -eq 1 ]; then
	./scripts/check_format.sh
fi
timing_exit check_format

$MAKE $MAKEFLAGS clean
if [ $SPDK_BUILD_SHARED_OBJECT -eq 1 ]; then
	./configure $config_params --with-shared
	$MAKE $MAKEFLAGS
	$MAKE $MAKEFLAGS clean
	report_test_completion "shared_object_build"
fi

scanbuild=''
make_timing_label='make'
if [ $SPDK_RUN_SCANBUILD -eq 1 ] && hash scan-build; then
	scanbuild="scan-build -o $out/scan-build-tmp --status-bugs"
	make_timing_label='scanbuild_make'
	report_test_completion "scanbuild"

fi

if [ $SPDK_RUN_VALGRIND -eq 1 ]; then
	report_test_completion "valgrind"
fi

if [ $SPDK_RUN_ASAN -eq 1 ]; then
	report_test_completion "asan"
fi

if [ $SPDK_RUN_UBSAN -eq 1 ]; then
	report_test_completion "ubsan"
fi

echo $scanbuild

timing_enter "$make_timing_label"
fail=0
./configure $config_params
time $scanbuild $MAKE $MAKEFLAGS || fail=1
if [ $fail -eq 1 ]; then
	if [ -d $out/scan-build-tmp ]; then
		scanoutput=$(ls -1 $out/scan-build-tmp/)
		mv $out/scan-build-tmp/$scanoutput $out/scan-build
		rm -rf $out/scan-build-tmp
		chmod -R a+rX $out/scan-build
	fi
	exit 1
else
	rm -rf $out/scan-build-tmp
fi
timing_exit "$make_timing_label"

# Check for generated files that are not listed in .gitignore
timing_enter generated_files_check
if [ `git status --porcelain --ignore-submodules | wc -l` -ne 0 ]; then
	echo "Generated files missing from .gitignore:"
	git status --porcelain
	exit 1
fi
timing_exit generated_files_check

# Check that header file dependencies are working correctly by
#  capturing a binary's stat data before and after touching a
#  header file and re-making.
timing_enter dependency_check
STAT1=`stat examples/nvme/identify/identify`
sleep 1
touch lib/nvme/nvme_internal.h
$MAKE $MAKEFLAGS
STAT2=`stat examples/nvme/identify/identify`

if [ "$STAT1" == "$STAT2" ]; then
	echo "Header dependency check failed"
	exit 1
fi
timing_exit dependency_check

# Test 'make install'
timing_enter make_install
rm -rf /tmp/spdk
mkdir /tmp/spdk
$MAKE $MAKEFLAGS install DESTDIR=/tmp/spdk prefix=/usr
ls -lR /tmp/spdk
rm -rf /tmp/spdk
timing_exit make_install

timing_enter doxygen
if [ $SPDK_BUILD_DOC -eq 1 ] && hash doxygen; then
	$MAKE -C "$rootdir"/doc --no-print-directory $MAKEFLAGS &> "$out"/doxygen.log
	if [ -s "$out"/doxygen.log ]; then
		cat "$out"/doxygen.log
		echo "Doxygen errors found!"
		exit 1
	fi
	if hash pdflatex 2>/dev/null; then
		$MAKE -C "$rootdir"/doc/output/latex --no-print-directory $MAKEFLAGS &>> "$out"/doxygen.log
	fi
	mkdir -p "$out"/doc
	mv "$rootdir"/doc/output/html "$out"/doc
	if [ -f "$rootdir"/doc/output/latex/refman.pdf ]; then
		mv "$rootdir"/doc/output/latex/refman.pdf "$out"/doc/spdk.pdf
	fi
	$MAKE -C "$rootdir"/doc --no-print-directory $MAKEFLAGS clean &>> "$out"/doxygen.log
	if [ -s "$out"/doxygen.log ]; then
		rm "$out"/doxygen.log
	fi
	rm -rf "$rootdir"/doc/output
fi
timing_exit doxygen

timing_exit autobuild
