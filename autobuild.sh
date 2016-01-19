#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0))
source "$rootdir/scripts/autotest_common.sh"

out=$PWD

umask 022

cd $rootdir

timing_enter autobuild

timing_enter check_format
./scripts/check_format.sh
timing_exit check_format

timing_enter build_kmod
./scripts/build_kmod.sh build
timing_exit build_kmod

scanbuild=''
if hash scan-build; then
	scanbuild="scan-build -o $out/scan-build-tmp --status-bugs"
fi

$MAKE $MAKEFLAGS clean

timing_enter scanbuild_make
fail=0
time $scanbuild $MAKE $MAKEFLAGS DPDK_DIR=$DPDK_DIR $MAKECONFIG || fail=1
timing_exit scanbuild_make

# Check that header file dependencies are working correctly by
#  capturing a binary's stat data before and after touching a
#  header file and re-making.
STAT1=`stat examples/nvme/identify/identify`
sleep 1
touch lib/nvme/nvme_internal.h
$MAKE $MAKEFLAGS DPDK_DIR=$DPDK_DIR $MAKECONFIG || fail=1
STAT2=`stat examples/nvme/identify/identify`

if [ "$STAT1" == "$STAT2" ]; then
	fail=1
fi

if [ -d $out/scan-build-tmp ]; then
	scanoutput=$(ls -1 $out/scan-build-tmp/)
	mv $out/scan-build-tmp/$scanoutput $out/scan-build
	rm -rf $out/scan-build-tmp
	chmod -R a+rX $out/scan-build
fi

timing_enter doxygen
if hash doxygen; then
	(cd "$rootdir"/doc; $MAKE $MAKEFLAGS)
	mkdir -p "$out"/doc
	for d in "$rootdir"/doc/output.*; do
		component=$(basename "$d" | sed -e 's/^output.//')
		mv "$d"/html "$out"/doc/$component
		rm -rf "$d"
	done
fi
timing_exit doxygen

timing_exit autobuild

exit $fail
