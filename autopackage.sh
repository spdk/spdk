#!/usr/bin/env bash

set -e

# If the configuration of tests is not provided, no tests will be carried out.
if [[ ! -f $1 ]]; then
	echo "ERROR: SPDK test configuration not specified"
	exit 1
fi

source "$1"

rootdir=$(readlink -f $(dirname $0))
testdir=$rootdir # to get the storage space for tests
source "$rootdir/test/common/autotest_common.sh"

function build_rpms() (
	local version rpms

	# Make sure linker will not attempt to look under DPDK's repo dir to get the libs
	unset -v LD_LIBRARY_PATH

	install_uninstall_rpms() {
		rpms=("$builddir/rpm/x86_64/"*.rpm)

		sudo rpm -i "${rpms[@]}"
		rpms=("${rpms[@]##*/}") rpms=("${rpms[@]%.rpm}")
		# Check if we can find one of the apps in the PATH now and verify if it doesn't miss
		# any libs.
		LIST_LIBS=yes "$rootdir/rpmbuild/rpm-deps.sh" "${SPDK_APP[@]##*/}"
		sudo rpm -e "${rpms[@]}"
	}

	build_rpm() {
		# Separate run to see the final .spec in use
		GEN_SPEC=yes BUILDDIR=$builddir MAKEFLAGS="$MAKEFLAGS" SPDK_VERSION="$version" DEPS=no "$rootdir/rpmbuild/rpm.sh" "$@"
		# Actual build
		BUILDDIR=$builddir MAKEFLAGS="$MAKEFLAGS" SPDK_VERSION="$version" DEPS=no "$rootdir/rpmbuild/rpm.sh" "$@"
		install_uninstall_rpms
	}

	build_rpm_with_rpmed_dpdk() {
		sudo dnf install -y dpdk-devel
		build_rpm --with-shared --with-dpdk
	}

	version="test_shared"
	builddir=$SPDK_TEST_STORAGE/test-rpm

	run_test "build_shared_rpm" build_rpm --with-shared
	run_test "build_shared_rpm_with_rpmed_dpdk" build_rpm_with_rpmed_dpdk

	if [[ -n $SPDK_TEST_NATIVE_DPDK ]]; then
		version="test_shared_native_dpdk"
		run_test "build_shared_native_dpdk_rpm" build_rpm --with-shared --with-dpdk="$SPDK_RUN_EXTERNAL_DPDK"
	fi
)

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

if [[ $SPDK_TEST_RELEASE_BUILD -eq 1 ]]; then
	run_test "build_rpms" build_rpms
	$MAKE clean
fi

if [[ $RUN_NIGHTLY -eq 0 ]]; then
	timing_finish
	exit 0
fi

timing_enter build_release

config_params="$(get_config_params | sed 's/--enable-debug//g')"
if [ $(uname -s) = Linux ]; then
	# LTO needs a special compiler to work under clang. See detect_cc.sh for details.
	if [[ $CC == *clang* ]]; then
		LD=$(type -P ld.gold)
		export LD
	fi
	./configure $config_params --enable-lto
else
	# LTO needs a special compiler to work on BSD.
	./configure $config_params
fi
$MAKE ${MAKEFLAGS}
$MAKE ${MAKEFLAGS} clean

timing_exit build_release

timing_finish
