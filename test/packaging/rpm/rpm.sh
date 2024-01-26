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
BUILDDIR=$builddir
DEPS=no
arch=$(uname -m)

export MAKEFLAGS BUILDDIR DEPS

cleanup() {
	rm -rf "$builddir"
	rm -rf "$(rpm --eval "%{_topdir}")"
	rm -rf /tmp/spdk-test_gen_spec
	sudo rpm -e $(rpm -qa | grep -E 'spdk|dpdk') || true
}

gen_dpdk_spec() (
	cat <<- DPDK_SPEC
		%define source_date_epoch_from_changelog %{nil}

		Name:           dpdk-devel
		Version:        $1
		Release:        $2
		Summary:        TESTME
		Source:         dpdk.tar.gz
		License:        TESTME

		%description
		TESTME

		%files
		/usr/local/lib/*
	DPDK_SPEC
)

build_dpdk_rpm() (
	local dpdkdir=$1 version=${2%.*} spec=$builddir/dpdk.spec
	local dpdkbuildroot=$builddir/dpdk/usr/local/lib dep
	local srcdir=$builddir/source
	local rpmdir=$builddir/rpms
	local release=1

	mkdir -p "$srcdir" "$rpmdir"

	# Dummy package to satisfy rpmbuild
	: > "$srcdir/dpdk.tar.gz"

	gen_dpdk_spec "$version" "$release" > "$spec"

	# Prepare our buildroot to pack just the libraries without actually building
	# anything. To do so, we need to copy what we need into a separate location
	# to not fiddle with rpmbuild's view on what should be packed and what
	# shouldn't.
	mkdir -p "$dpdkbuildroot"

	[[ -e $dpdkdir/lib ]]
	cp -a "$dpdkdir/lib/"* "$dpdkbuildroot/"

	# Check isa-l and IPSec dependencies - dedicated to the vs-dpdk test
	for dep in isa-l/build/lib intel-ipsec-mb; do
		[[ -e $dpdkdir/../$dep ]] || continue
		find "$dpdkdir/../$dep" \
			-name '*.so*' \
			-exec cp -at "$dpdkbuildroot/" {} +
	done

	rpmbuild \
		--buildroot="$builddir/dpdk" \
		-D "_rpmdir $rpmdir" \
		-D "_sourcedir $srcdir" \
		--noclean \
		--nodebuginfo \
		-ba "$spec"

	# Put our dummy package in place
	sudo rpm -i "$rpmdir/$arch/dpdk-devel-$version-$release.$arch.rpm"

	# Run actual test
	MV_RUNPATH=$dpdkdir build_rpm --with-shared --with-dpdk="$dpdkdir"

	sudo rpm -e dpdk-devel
)

install_uninstall_rpms() {
	local rpms

	rpms=("${1:-$builddir/rpm/}/$arch/"*.rpm)

	# Clean repo first to make sure linker won't follow $SPDK_APP's RUNPATH
	make -C "$rootdir" clean $MAKEFLAGS

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
	# FIXME: Remove the MV_RUNPATH HACK when the patchelf is available. See: 9cb9f24152f
	if [[ -n $MV_RUNPATH ]]; then
		mv "$MV_RUNPATH"{,.hidden}
	fi
	install_uninstall_rpms
	if [[ -n $MV_RUNPATH ]]; then
		mv "$MV_RUNPATH"{.hidden,}
	fi
}

build_shared_rpm() {
	build_rpm --with-shared
}

build_rpm_with_rpmed_dpdk() {
	local es=0 dpdk_rpms=()

	dpdk_rpms=(/var/spdk/dependencies/autotest/dpdk/dpdk?(-devel).rpm)
	if ((${#dpdk_rpms[@]} == 2)); then # dpdk, dpdk-devel
		echo "INFO: Installing DPDK from local package: $(rpm -q --queryformat="%{VERSION}" "${dpdk_rpms[0]}")" >&2
		sudo rpm -i "${dpdk_rpms[@]}"
	else
		echo "WARNING: No local packages found, trying to install DPDK from the remote" >&2
		sudo dnf install -y dpdk-devel
	fi
	build_rpm --with-shared --with-dpdk || es=$?

	if ((es == 11)); then
		echo "ERROR: Failed to resolve required build dependencies. Please review the build log." >&2
	fi

	sudo rpm -e dpdk{,-devel} || true

	return "$es"
}

build_rpm_from_gen_spec() {
	local version=test_gen_spec
	local sourcedir rpmdir rpmbuilddir

	GEN_SPEC=yes \
		USE_DEFAULT_DIRS=yes \
		SPDK_VERSION="$version" \
		"$rootdir/rpmbuild/rpm.sh" --with-shared > "$builddir/gen-spdk.spec"

	# Default locations should be in use.
	sourcedir=$(rpm --eval "%{_sourcedir}")
	rpmdir=$(rpm --eval "%{_rpmdir}")
	rpmbuilddir=$(rpm --eval "%{_builddir}")

	mkdir -p "$sourcedir" "$rpmdir" "$rpmbuilddir"

	# Prepare the source at the default location - default %prep step requires
	# extra dir inside the source package.
	cp -r "$rootdir" "/tmp/spdk-$version"
	tar -czf "$sourcedir/spdk-$version.tar.gz" -C /tmp "spdk-$version"

	# See rpm.sh for details on the PYTHONPATH HACK
	PYTHONPATH="$(python3 -c "import sys; print('%s' % ':'.join(sys.path)[1:])")" \
		rpmbuild -ba "$builddir/gen-spdk.spec"
	# Clean builddir to make sure linker won't follow $SPDK_APP's RUNPATH
	rm -rf "$rpmbuilddir"
	install_uninstall_rpms "$rpmdir"
}

build_shared_native_dpdk_rpm() {
	[[ -e /tmp/spdk-ld-path ]] # autobuild dependency
	source /tmp/spdk-ld-path

	build_dpdk_rpm \
		"$SPDK_RUN_EXTERNAL_DPDK" \
		"$(< $SPDK_RUN_EXTERNAL_DPDK/../VERSION)"
}

trap 'cleanup' EXIT

run_test "build_shared_rpm" build_shared_rpm
run_test "build_rpm_from_gen_spec" build_rpm_from_gen_spec

if ((RUN_NIGHTLY == 1)); then
	run_test "build_shared_rpm_with_rpmed_dpdk" build_rpm_with_rpmed_dpdk
fi

if [[ -n $SPDK_TEST_NATIVE_DPDK ]]; then
	run_test "build_shared_native_dpdk_rpm" build_shared_native_dpdk_rpm
fi
