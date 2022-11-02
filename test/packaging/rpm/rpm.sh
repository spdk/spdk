#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation
#  All rights reserved.
#

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../../")
source "$rootdir/test/common/autotest_common.sh"

builddir=$SPDK_TEST_STORAGE/test-rpm

# Make sure linker will not attempt to look under DPDK's repo dir to get the libs
unset -v LD_LIBRARY_PATH

# Export some common settings
MAKEFLAGS="-j $(nproc)"
BUILDDIR=$builddir
DEPS=no

export MAKEFLAGS BUILDDIR DEPS

install_uninstall_rpms() {
	local rpms

	rpms=("${1:-$builddir/rpm/}/x86_64/"*.rpm)

	sudo rpm -i "${rpms[@]}"
	# Check if we can find one of the apps in the PATH now and verify if it doesn't miss
	# any libs.
	LIST_LIBS=yes "$rootdir/rpmbuild/rpm-deps.sh" "${SPDK_APP[@]##*/}"
	rm "${rpms[@]}"
	rpms=("${rpms[@]##*/}") rpms=("${rpms[@]%.rpm}")
	sudo rpm -e "${rpms[@]}"
}

build_rpm() {
	# Separate run to see the final .spec in use
	GEN_SPEC=yes "$rootdir/rpmbuild/rpm.sh" "$@"
	# Actual build
	"$rootdir/rpmbuild/rpm.sh" "$@" || return $?
	install_uninstall_rpms
}

build_shared_rpm() {
	build_rpm --with-shared
}

build_rpm_with_rpmed_dpdk() {
	local es=0

	sudo dnf install -y dpdk-devel
	build_rpm --with-shared --with-dpdk || es=$?

	if ((es == 11)); then
		echo "ERROR: Failed to resolve required build dependencies. Please review the build log." >&2
	fi
	return "$es"
}

build_rpm_from_gen_spec() {
	local version=test_gen_spec
	local sourcedir rpmdir

	GEN_SPEC=yes \
		USE_DEFAULT_DIRS=yes \
		SPDK_VERSION="$version" \
		"$rootdir/rpmbuild/rpm.sh" --with-shared > "$builddir/gen-spdk.spec"

	# Default locations should be in use.
	sourcedir=$(rpm --eval "%{_sourcedir}") rpmdir=$(rpm --eval "%{_rpmdir}")
	mkdir -p "$sourcedir" "$rpmdir"

	# Prepare the source at the default location - default %prep step requires
	# extra dir inside the source package hence the dance with symlinking to
	# the repo (after the extraction source should be under spdk-$version/) -
	# make sure symlinking is done outside of the repo to avoid nasty loops.
	ln -s "$rootdir" "/tmp/spdk-$version"
	tar -czhf "$sourcedir/spdk-$version.tar.gz" -C /tmp "spdk-$version"

	# See rpm.sh for details on the PYTHONPATH HACK
	PYTHONPATH="$(python3 -c "import sys; print('%s' % ':'.join(sys.path)[1:])")" \
		rpmbuild -ba "$builddir/gen-spdk.spec"
	install_uninstall_rpms "$rpmdir"
}

build_shared_native_dpdk_rpm() {
	build_rpm --with-shared --with-dpdk="$SPDK_RUN_EXTERNAL_DPDK"
}

run_test "build_shared_rpm" build_shared_rpm
if ((RUN_NIGHTLY == 1)); then
	run_test "build_shared_rpm_with_rpmed_dpdk" build_rpm_with_rpmed_dpdk
	run_test "build_rpm_from_gen_spec" build_rpm_from_gen_spec
	if [[ -n $SPDK_TEST_NATIVE_DPDK ]]; then
		run_test "build_shared_native_dpdk_rpm" build_shared_native_dpdk_rpm
	fi
fi

rm -rf "$builddir"
