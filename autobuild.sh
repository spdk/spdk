#!/usr/bin/env bash

set -e

src=$(readlink -f $(dirname $0))
out=$PWD

MAKEFLAGS=${MAKEFLAGS:--j16}
DPDK_DIR=/usr/local/dpdk-2.1.0/x86_64-native-linuxapp-gcc

cd $src

scanbuild=''
if hash scan-build; then
	scanbuild="scan-build -o $out/scan-build-tmp"
fi

make $MAKEFLAGS clean
time $scanbuild make $MAKEFLAGS DPDK_DIR=$DPDK_DIR

if [ -d $out/scan-build-tmp ]; then
	scanoutput=$(ls -1 $out/scan-build-tmp/)
	mv $out/scan-build-tmp/$scanoutput $out/scan-build
	rmdir $out/scan-build-tmp
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
