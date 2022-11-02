#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation
#  All rights reserved.
#

set -e

specdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$specdir/../")

[[ -e /etc/os-release ]]
source /etc/os-release

id_ok=no

for id in $ID $ID_LIKE; do
	[[ "$id" =~ ^(fedora|centos|rhel) ]] && id_ok=yes
done

if [[ "$id_ok" != "yes" ]]; then
	printf '%s not supported\n' "$ID" >&2
	exit 1
fi

get_config() {
	# Intercept part of the ./configure's cmdline we are interested in
	configure_opts=($(getopt -l "$1::" -o "" -- $configure 2> /dev/null)) || true
	# If "--" is the first argument then either the cmdline is empty or doesn't
	# match on what we are looking for. In either case simply return as there
	# is nothing to check.
	[[ ${configure_opts[0]} == "--" ]] && return 1

	if [[ $2 == has-arg ]]; then
		[[ -n ${configure_opts[1]} && ${configure_opts[1]} != "''" ]]
	elif [[ $2 == print ]]; then
		echo "${configure_opts[1]//\'/}"
	fi
}

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

build_macros() {
	local -g macros=()
	local dir _dir

	macros+=(-D "configure ${configure:-"%{nil}"}")
	macros+=(-D "make $make")
	macros+=(-D "release $release")
	macros+=(-D "version $version")

	# Adjust dir macros to update the final location of the RPMS
	for dir in build buildroot rpm source spec srcrpm; do
		_dir=$(rpm --eval "%{_${dir}dir}")
		if [[ -z $USE_DEFAULT_DIRS ]]; then
			macros+=(-D "_${dir}dir $rpmbuild_dir/$dir")
			_dir=$rpmbuild_dir/$dir
		fi
		local -g "_${dir}dir=$_dir"
	done

	if get_config with-shared; then
		macros+=(-D "shared 1")
		macros+=(-D "dpdk 1")
	fi

	if get_config with-dpdk; then
		if ! get_config with-dpdk has-arg; then
			# spdk is requested to build against installed dpdk (i.e. provided by the dist).
			# Don't build dpdk rpm rather define proper requirements for the spdk.
			macros+=(-D "dpdk 0")
			macros+=(-D "shared 1")
			# This maps how Epoch is used inside dpdk packages across different distros. It's
			# mainly relevant when comparing version of required packages. Default maps to 0.
			local -A dpdk_rpm_epoch["fedora"]=2
			local dpdk_version_min=${dpdk_rpm_epoch["$ID"]:-0}:20.11
			local dpdk_req="dpdk-devel >= $dpdk_version_min"

			requirements=${requirements:+$requirements, }"$dpdk_req"
			build_requirements=${build_requirements:+$build_requirements, }"$dpdk_req"
		else
			dpdk_build_path=$(get_config with-dpdk print)
			dpdk_path=$(dirname "$dpdk_build_path")
			macros+=(-D "dpdk_build_path $dpdk_build_path")
			macros+=(-D "dpdk_path $dpdk_path")
		fi
	fi

	if get_config with-rbd; then
		macros+=(-D "rbd 1")
		requirements=${requirements:+$requirements, }"librados2, librbd1"
		build_requirements=${build_requirements:+$build_requirements, }"librados-devel, librbd-devel"
	fi

	if get_config libdir has-arg; then
		macros+=(-D "libdir $(get_config libdir print)")
	fi

	if get_config with-vfio-user; then
		macros+=(-D "vfio_user 1")
	fi

	if [[ $deps == no ]]; then
		macros+=(-D "deps 0")
	fi

	if [[ -n $requirements ]]; then
		macros+=(-D "requirements 1")
		macros+=(-D "requirements_list $requirements")
	fi

	if [[ -n $build_requirements ]]; then
		macros+=(-D "build_requirements 1")
		macros+=(-D "build_requirements_list $build_requirements")
	fi

	build_macros_flags
}

build_macros_flags() {
	local flags flag

	flags=(CFLAGS CXXFLAGS LDFLAGS)

	for flag in "${flags[@]}"; do
		# If we are running in the environment where the flag is set, don't touch it -
		# rpmbuild will use it as is during the build. If it's not set, make sure the
		# rpmbuild won't set its defaults which may affect the build in an unpredictable
		# manner.
		[[ -n ${!flag} ]] && continue
		macros+=(-D "build_${flag,,} %{nil}")
	done
}

gen_spec() {
	rpmspec "${macros[@]}" -P "$spec"
}

build_rpm() (
	fedora_python_sys_path_workaround

	mkdir -p \
		"$_builddir" \
		"$_buildrootdir" \
		"$_rpmdir" \
		"$_sourcedir" \
		"$_specdir" \
		"$_srcrpmdir"

	# Despite building in-place, rpmbuild still looks under %{_sourcedir} as defined
	# in Source:. Create a dummy file to fulfil its needs and to keep Source in
	# the .spec.
	: > "$_sourcedir/spdk-$version.tar.gz"

	cd "$rootdir"

	printf '* Starting rpmbuild...\n'
	rpmbuild --clean --nodebuginfo "${macros[@]}" --build-in-place -ba "$spec"
)

# .spec defaults
configure=$*
deps=${DEPS:-yes}
make="${MAKEFLAGS:--j $(nproc)}"
release=${RPM_RELEASE:-1}
requirements=${REQUIREMENTS:-}
build_requirements=${BUILD_REQUIREMENTS:-}
version=${SPDK_VERSION:-$(get_version)}

rpmbuild_dir=${BUILDDIR:-"$HOME/rpmbuild"}
spec=$specdir/spdk.spec

build_macros
if [[ -n $GEN_SPEC ]]; then
	gen_spec
	exit 0
fi
build_rpm
