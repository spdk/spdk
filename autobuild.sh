#!/usr/bin/env bash

set -e

# If the configuration of tests is not provided, no tests will be carried out.
if [[ ! -f $1 ]]; then
	echo "ERROR: SPDK test configuration not specified"
	exit 1
fi

source "$1"

rootdir=$(readlink -f $(dirname $0))
source "$rootdir/test/common/autotest_common.sh"

out=$PWD

umask 022

cd $rootdir

date -u
git describe --tags

if [ "$SPDK_TEST_OCF" -eq 1 ]; then
	# We compile OCF sources ourselves
	# They don't need to be checked with scanbuild and code coverage is not applicable
	# So we precompile OCF now for further use as standalone static library
	./configure $(echo $config_params | sed 's/--enable-coverage//g')
	$MAKE $MAKEFLAGS include/spdk/config.h
	CC=gcc CCAR=ar $MAKE $MAKEFLAGS -C lib/env_ocf exportlib O=$rootdir/build/ocf.a
	# Set config to use precompiled library
	config_params="$config_params --with-ocf=/$rootdir/build/ocf.a"
fi

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

$MAKE $MAKEFLAGS clean
if [ $SPDK_BUILD_SHARED_OBJECT -eq 1 ]; then
	$rootdir/test/make/check_so_deps.sh
	report_test_completion "shared_object_build"
fi

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
if [ $(git status --porcelain --ignore-submodules | wc -l) -ne 0 ]; then
	echo "Generated files missing from .gitignore:"
	git status --porcelain --ignore-submodules
	exit 1
fi
timing_exit generated_files_check

# Check that header file dependencies are working correctly by
#  capturing a binary's stat data before and after touching a
#  header file and re-making.
timing_enter dependency_check
STAT1=$(stat examples/nvme/identify/identify)
sleep 1
touch lib/nvme/nvme_internal.h
$MAKE $MAKEFLAGS
STAT2=$(stat examples/nvme/identify/identify)

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
timing_exit make_install

# Test 'make uninstall'
timing_enter make_uninstall
# Create empty file to check if it is not deleted by target uninstall
touch /tmp/spdk/usr/lib/sample_xyz.a
$MAKE $MAKEFLAGS uninstall DESTDIR=/tmp/spdk prefix=/usr
if [[ $(find /tmp/spdk/usr -maxdepth 1 -mindepth 1 | wc -l) -ne 2 ]] || [[ $(find /tmp/spdk/usr/lib/ -maxdepth 1 -mindepth 1 | wc -l) -ne 1 ]]; then
	ls -lR /tmp/spdk
	rm -rf /tmp/spdk
	echo "Make uninstall failed"
	exit 1
else
	rm -rf /tmp/spdk
fi
timing_exit make_uninstall

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
