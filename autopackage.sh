#!/usr/bin/env bash

set -xe

# If the configuration of tests is not provided, no tests will be carried out.
if [[ ! -f $1 ]]; then
	echo "ERROR: SPDK test configuration not specified"
	exit 1
fi

source "$1"

rootdir=$(readlink -f $(dirname $0))
source "$rootdir/test/common/autotest_common.sh"

out=$PWD

MAKEFLAGS=${MAKEFLAGS:--j16}
cd $rootdir

timing_enter porcelain_check
$MAKE clean

if [ $(git status --porcelain --ignore-submodules | wc -l) -ne 0 ]; then
	echo make clean left the following files:
	git status --porcelain --ignore-submodules
	exit 1
fi
timing_exit porcelain_check

if [ $RUN_NIGHTLY -eq 0 ]; then
	timing_finish
	exit 0
fi

timing_enter autopackage

spdk_pv=spdk-$(date +%Y_%m_%d)
spdk_tarball=${spdk_pv}.tar
dpdk_pv=dpdk-$(date +%Y_%m_%d)
dpdk_tarball=${dpdk_pv}.tar
ipsec_pv=ipsec-$(date +%Y_%m_%d)
ipsec_tarball=${ipsec_pv}.tar
isal_pv=isal-$(date +%Y_%m_%d)
isal_tarball=${isal_pv}.tar
ocf_pv=ocf-$(date +%Y_%m_%d)
ocf_tarball=${ocf_pv}.tar
snap_pv=snap-$(date +%Y_%m_%d)
snap_tarball=${snap_pv}.tar

find . -iname "spdk-*.tar* dpdk-*.tar* ipsec-*.tar* isal-*.tar* snap-*.tar*" -delete
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

if [ -d "intel-ipsec-mb" ]; then
	cd intel-ipsec-mb
	git archive HEAD^{tree} --prefix=intel-ipsec-mb/ -o ../${ipsec_tarball}
	cd ..
	tar -C "$tmpdir/${spdk_pv}" -xf $ipsec_tarball
fi

if [ -d "isa-l" ]; then
	cd isa-l
	git archive HEAD^{tree} --prefix=isa-l/ -o ../${isal_tarball}
	cd ..
	tar -C "$tmpdir/${spdk_pv}" -xf $isal_tarball
fi

if [ -d "ocf" ]; then
	cd ocf
	git archive HEAD^{tree} --prefix=ocf/ -o ../${ocf_tarball}
	cd ..
	tar -C "$tmpdir/${spdk_pv}" -xf $ocf_tarball
fi

if [ -d "snap-rdma" ]; then
        cd snap-rdma 
        git archive HEAD^{tree} --prefix=snap-rdma/ -o ../${snap_tarball}
        cd ..
        tar -C "$tmpdir/${spdk_pv}" -xf $snap_tarball
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
