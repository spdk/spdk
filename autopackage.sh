#!/usr/bin/env bash

set -xe

rootdir=$(readlink -f $(dirname $0))
source "$rootdir/scripts/autotest_common.sh"

out=$PWD

MAKEFLAGS=${MAKEFLAGS:--j16}
cd $rootdir

timing_enter autopackage

$MAKE clean

if [ `git status --porcelain | wc -l` -ne 0 ]; then
	echo make clean left the following files:
	git status --porcelain
	exit 1
fi

pv=spdk-$(date +%Y_%m_%d)

find . -iname "spdk-*.tar.gz" -delete
git archive HEAD -9 --prefix=${pv}/ -o ${pv}.tar.gz

tarball=$(ls -1 spdk-*.tar.gz)

# Build from packaged source
tmpdir=$(mktemp -d)
echo "tmpdir=$tmpdir"
tar -C "$tmpdir" -xf $tarball
(
	cd "$tmpdir"/spdk-*
	./configure $config_params --disable-debug --enable-werror
	time $MAKE ${MAKEFLAGS}
)
rm -rf "$tmpdir"

timing_exit autopackage

timing_finish
