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

spdk_pv=spdk-$(date +%Y_%m_%d)
spdk_tarball=${spdk_pv}.tar
dpdk_pv=dpdk-$(date +%Y_%m_%d)
dpdk_tarball=${dpdk_pv}.tar

find . -iname "spdk-*.tar* dpdk-*.tar*" -delete
git archive HEAD^{tree} --prefix=${spdk_pv}/ -o ${spdk_tarball}

# Build from packaged source
tmpdir=$(mktemp -d)
echo "tmpdir=$tmpdir"
tar -C "$tmpdir" -xf $spdk_tarball

if [ -z "$WITH_DPDK_DIR" ]; then
	cd dpdk
	git archive HEAD^{tree} --prefix=dpdk/ -o ../${dpdk_tarball}
	cd ..
	tar -C "$tmpdir/${spdk_pv}" -xf $dpdk_tarball
fi

(
	cd "$tmpdir"/spdk-*
	# use $config_params to get the right dependency options, but disable coverage and ubsan
	#  explicitly since they are not needed for this build
	./configure $config_params --disable-debug --enable-werror --disable-coverage --disable-ubsan
	time $MAKE ${MAKEFLAGS}
)
rm -rf "$tmpdir"

timing_exit autopackage

timing_finish
