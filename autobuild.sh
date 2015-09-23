#!/usr/bin/env bash

set -e

src=$(readlink -f $(dirname $0))
out=$PWD

MAKEFLAGS=${MAKEFLAGS:--j16}
DPDK_DIR=/usr/local/dpdk-2.1.0/x86_64-native-linuxapp-gcc

cd $src

./scripts/check_format.sh

scanbuild=''
if hash scan-build; then
	scanbuild="scan-build -o $out/scan-build-tmp --status-bugs"
fi

make $MAKEFLAGS clean
fail=0
time $scanbuild make $MAKEFLAGS DPDK_DIR=$DPDK_DIR || fail=1

if [ -d $out/scan-build-tmp ]; then
	scanoutput=$(ls -1 $out/scan-build-tmp/)
	mv $out/scan-build-tmp/$scanoutput $out/scan-build
	rm -rf $out/scan-build-tmp
	chmod -R a+rX $out/scan-build
fi

if hash doxygen; then
	(cd "$src"/doc; make $MAKEFLAGS)
	mkdir -p "$out"/doc
	for d in "$src"/doc/output.*; do
		component=$(basename "$d" | sed -e 's/^output.//')
		mv "$d"/html "$out"/doc/$component
		rm -rf "$d"
	done
fi

exit $fail
