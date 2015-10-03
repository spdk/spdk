#!/usr/bin/env bash

set -e

src=$(readlink -f $(dirname $0))
source "$src/scripts/autotest_common.sh"

out=$PWD

umask 022

cd $src

./scripts/check_format.sh

scanbuild=''
if hash scan-build; then
	scanbuild="scan-build -o $out/scan-build-tmp --status-bugs"
fi

$MAKE $MAKEFLAGS clean
fail=0
time $scanbuild $MAKE $MAKEFLAGS DPDK_DIR=$DPDK_DIR || fail=1

# Check that header file dependencies are working correctly by
#  capturing a binary's stat data before and after touching a
#  header file and re-making.
STAT1=`stat examples/nvme/identify/identify`
touch lib/nvme/nvme_internal.h
$MAKE $MAKEFLAGS DPDK_DIR=$DPDK_DIR || fail=1
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

if hash doxygen; then
	(cd "$src"/doc; $MAKE $MAKEFLAGS)
	mkdir -p "$out"/doc
	for d in "$src"/doc/output.*; do
		component=$(basename "$d" | sed -e 's/^output.//')
		mv "$d"/html "$out"/doc/$component
		rm -rf "$d"
	done
fi

exit $fail
