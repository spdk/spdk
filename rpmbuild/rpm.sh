#!/usr/bin/env bash

set -e

specdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$specdir/../")

[[ -e /etc/os-release ]]
source /etc/os-release

if [[ $ID != fedora && $ID != centos && $ID != rhel ]]; then
	printf '%s not supported\n' "$ID" >&2
	exit 1
fi

fedora_python_sys_path_workaround() {
	[[ -z $NO_WORKAROUND ]] || return 0

	# Fedora builds its python version with a patch which attempts to remove all
	# "/usr/local" paths from sys.path in case it's run under RPM environment,
	# i.e., when RPM_BUILD_ROOT variable is detected. This particular variable
	# is set by the rpmbuild when it executes its sh wrappers built out of the
	# .spec file.

	# This is problematic in case meson and ninja were installed via rooted pip
	# which had its working directory set to /usr/local. As a result, when the
	# SPDK executes meson to build DPDK, from rpmbuild env, it fails as
	# it's not able to find its mesonbuild module.

	# To workaround this little hiccup we fetch the entire sys.path list and
	# then export it via PYTHONPATH so when rpmbuild kicks in, python will be
	# able to find all the modules regardless if the RPM_BUILD_ROOT is set or
	# not.
	# FIXME: The alternative is to unset RPM_BUILD_ROOT directly in the spec?
	# It does work but it feels wrong.

	PYTHONPATH="$(python3 -c "import sys; print('%s' % ':'.join(sys.path)[1:])")"
	export PYTHONPATH
}

get_version() {
	local version
	version=$(git -C "$rootdir" describe --tags --abbrev=0)

	echo "${version%%-*}"
}

build_rpm() (
	local macros=() dir

	macros+=(-D "configure $configure")
	macros+=(-D "make $make")
	macros+=(-D "release $release")
	macros+=(-D "version $version")

	# Adjust dir macros to update the final location of the RPMS
	for dir in build buildroot rpm source spec srcrpm; do
		mkdir -p "$rpmbuild_dir/$dir"
		macros+=(-D "_${dir}dir $rpmbuild_dir/$dir")
	done

	if [[ $configure == *"with-shared"* || $configure == *"with-dpdk"* ]]; then
		macros+=(-D "dpdk 1")
		macros+=(-D "shared 1")
	fi

	if [[ $configure == *"with-dpdk"* ]]; then
		dpdk_build_path=${configure#*with-dpdk=}
		dpdk_build_path=${dpdk_build_path%% *}
		dpdk_path=${dpdk_build_path%/*}
		macros+=(-D "dpdk_build_path $dpdk_build_path")
		macros+=(-D "dpdk_path $dpdk_path")
	fi

	if [[ $deps == no ]]; then
		macros+=(-D "deps 0")
	fi

	if [[ -n $requirements ]]; then
		macros+=(-D "requirements 1")
		macros+=(-D "requirements_list $requirements")
	fi

	cd "$rootdir"

	fedora_python_sys_path_workaround

	# Despite building in-place, rpmbuild still looks under source dir as defined
	# in Source:. Create a dummy file to fulfil its needs and to keep Source in
	# the .spec.
	: > "$rpmbuild_dir/source/spdk-$version.tar.gz"

	printf '* Starting rpmbuild...\n'
	rpmbuild --clean --nodebuginfo "${macros[@]}" --build-in-place -ba "$spec"
)

# .spec defaults
configure=${*:-"%{nil}"}
deps=${DEPS:-yes}
make="${MAKEFLAGS:--j $(nproc)}"
release=${RPM_RELEASE:-1}
requirements=${REQUIREMENTS:-}
version=${SPDK_VERSION:-$(get_version)}

rpmbuild_dir=${BUILDDIR:-"$HOME/rpmbuild"}
spec=$specdir/spdk.spec

build_rpm
