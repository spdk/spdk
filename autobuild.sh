#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))
source "$rootdir/scripts/autotest_common.sh"

out=$PWD

umask 022

cd $rootdir

date -u
git describe --tags

timing_enter autobuild

./configure $config_params

timing_enter check_format
if [ $SPDK_RUN_CHECK_FORMAT -eq 1 ]; then
	./scripts/check_format.sh
fi
timing_exit check_format

timing_enter build_kmod
if [ $SPDK_BUILD_IOAT_KMOD -eq 1 ]; then
	./scripts/build_kmod.sh build
fi
timing_exit build_kmod

scanbuild=''
if [ $SPDK_RUN_SCANBUILD -eq 1 ] && hash scan-build; then
	scanbuild="scan-build -o $out/scan-build-tmp --status-bugs"
fi
echo $scanbuild
$MAKE $MAKEFLAGS clean

timing_enter scanbuild_make
fail=0
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
timing_exit scanbuild_make

# Check for generated files that are not listed in .gitignore
if [ `git status --porcelain | wc -l` -ne 0 ]; then
	echo "Generated files missing from .gitignore:"
	git status --porcelain
	exit 1
fi

# Check that header file dependencies are working correctly by
#  capturing a binary's stat data before and after touching a
#  header file and re-making.
STAT1=`stat examples/nvme/identify/identify`
sleep 1
touch lib/nvme/nvme_internal.h
$MAKE $MAKEFLAGS
STAT2=`stat examples/nvme/identify/identify`

if [ "$STAT1" == "$STAT2" ]; then
	echo "Header dependency check failed"
	exit 1
fi


timing_enter doxygen
if [ $SPDK_BUILD_DOC -eq 1 ] && hash doxygen; then
	(cd "$rootdir"/doc; $MAKE $MAKEFLAGS) &> "$out"/doxygen.log
	if hash pdflatex; then
		(cd "$rootdir"/doc/output/latex && $MAKE $MAKEFLAGS) &>> "$out"/doxygen.log
	fi
	mkdir -p "$out"/doc
	mv "$rootdir"/doc/output/html "$out"/doc
	if [ -f "$rootdir"/doc/output/latex/refman.pdf ]; then
		mv "$rootdir"/doc/output/latex/refman.pdf "$out"/doc/spdk.pdf
	fi
	(cd "$rootdir"/doc; $MAKE $MAKEFLAGS clean) &>> "$out"/doxygen.log
	rm -rf "$rootdir"/doc/output
fi
timing_exit doxygen

timing_exit autobuild
