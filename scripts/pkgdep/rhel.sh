#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#

disclaimer() {
	case "$ID" in
		rhel)
			cat <<- WARN

				WARNING: $PRETTY_NAME system detected.

				Please, note that the support for this platform is considered to be "best-effort",
				as in, access to some packages may be limited and/or missing. Review your repo
				setup to make sure installation of all dependencies is possible.

			WARN

			# Don't trigger errexit, simply install what's available. This is default
			# behavior of older yum versions (e.g. the one present on RHEL 7.x) anyway.
			yum() { "$(type -P yum)" --skip-broken "$@"; }
			# For systems which are not registered, subscription-manager will most likely
			# fail on most calls so simply ignore its failures.
			sub() { subscription-manager "$@" || :; }
			;;
		rocky)
			[[ $VERSION_ID == 8* ]] || return 0
			yum() { "$(type -P yum)" --setopt=skip_if_unavailable=True "$@"; }
			;;
	esac
}

is_repo() { yum repolist --all | grep -q "^$1"; }

disclaimer

if [[ $ID == centos && $VERSION_ID =~ ^[78].* ]]; then
	printf 'Not supported distribution detected (%s):(%s), aborting\n' "$ID" "$VERSION_ID" >&2
	exit 1
fi

# First, add extra EPEL, ELRepo, Ceph repos to have a chance of covering most of the packages
# on the enterprise systems, like RHEL.
if [[ $ID == centos || $ID == rhel || $ID == rocky ]]; then
	repos=() enable=("epel" "elrepo" "elrepo-testing") add=()

	if [[ $VERSION_ID == 8* ]]; then
		repos+=("https://dl.fedoraproject.org/pub/epel/epel-release-latest-8.noarch.rpm")
		repos+=("https://www.elrepo.org/elrepo-release-8.el8.elrepo.noarch.rpm")
		add+=("https://packages.daos.io/v2.0/EL8/packages/x86_64/daos_packages.repo")
		enable+=("daos-packages")
	fi

	if [[ $VERSION_ID == 9* ]]; then
		repos+=("https://www.elrepo.org/elrepo-release-9.el9.elrepo.noarch.rpm")
		repos+=("https://dl.fedoraproject.org/pub/epel/epel-release-latest-9.noarch.rpm")
		[[ $ID != rhel ]] && enable+=("crb")
		[[ $ID == centos ]] && enable+=("extras-common")
	fi

	if [[ $VERSION_ID == 10* ]]; then
		repos+=("https://www.elrepo.org/elrepo-release-10.el10.elrepo.noarch.rpm")
		repos+=("https://dl.fedoraproject.org/pub/epel/epel-release-latest-10.noarch.rpm")
		[[ $ID != rhel ]] && enable+=("crb")
		[[ $ID == centos ]] && enable+=("extras-common")
	fi

	# Add PowerTools needed for install CUnit-devel
	if [[ $ID == rocky && $VERSION_ID =~ ^[89].* ]]; then
		is_repo "PowerTools" && enable+=("PowerTools")
		is_repo "powertools" && enable+=("powertools")
		repos+=("centos-release-ceph-pacific.noarch")
		enable+=("centos-ceph-pacific")
	fi

	[[ $ID == rhel && $VERSION_ID == 8* ]] && repos+=("https://download.ceph.com/rpm-pacific/el8/noarch/ceph-release-1-1.el8.noarch.rpm")
	[[ $ID == rhel && $VERSION_ID == 9* ]] && repos+=("https://download.ceph.com/rpm-reef/el9/noarch/ceph-release-1-1.el9.noarch.rpm")

	if [[ $ID == rocky ]]; then
		enable+=("devel" "extras")
	fi

	if ((${#add[@]} > 0)); then
		yum install -y yum-utils
		for _repo in "${add[@]}"; do
			yum-config-manager --add-repo "$_repo"
		done
	fi

	if ((${#repos[@]} > 0)); then
		yum install -y "${repos[@]}" yum-utils
		yum-config-manager --enable "${enable[@]}"
	fi
	# Potential dependencies can be needed from other RHEL repos, enable them
	if [[ $ID == rhel ]]; then
		[[ $VERSION_ID == 8* ]] && sub repos --enable codeready-builder-for-rhel-8-x86_64-rpms
		[[ $VERSION_ID == 9* ]] && sub repos --enable codeready-builder-for-rhel-9-x86_64-rpms
	fi
fi

yum install -y gcc gcc-c++ make CUnit-devel libaio-devel openssl-devel \
	libuuid-devel ncurses-devel json-c-devel libcmocka-devel \
	clang clang-devel python3-pip unzip keyutils keyutils-libs-devel fuse3-devel patchelf \
	pkgconfig

[[ $VERSION_ID != 10* ]] && yum install -y libiscsi-devel

# Minimal install
# workaround for arm: ninja fails with dep on skbuild python module
if [ "$(uname -m)" = "aarch64" ]; then
	pip3 install scikit-build
fi

if echo "$ID $VERSION_ID" | grep -E -q 'rhel 8|rocky 8'; then
	yum install -y python36 python36-devel
	#Create hard link to use in SPDK as python
	if [[ ! -e /usr/bin/python && -e /etc/alternatives/python3 ]]; then
		ln -s /etc/alternatives/python3 /usr/bin/python
	fi
	# pip3, which is shipped with centos8 and rocky8, is currently providing faulty ninja binary
	# which segfaults at each run. To workaround it, upgrade pip itself and then use it for each
	# package - new pip will provide ninja at the same version but with the actually working
	# binary.
	pip3 install --upgrade pip
	pip3() { /usr/local/bin/pip "$@"; }
else
	yum install -y python python3-devel
fi

pip3 install -r "$rootdir/scripts/pkgdep/requirements.txt"

# Additional dependencies for SPDK CLI
yum install -y python3-configshell python3-pexpect
# Additional dependencies for ISA-L used in compression
yum install -y autoconf automake libtool help2man
# Additional dependencies for DPDK
yum install -y numactl-devel nasm
# Additional dependencies for USDT
yum install -y systemtap-sdt-devel
if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
	# Tools for developers
	devtool_pkgs=(git sg3_utils pciutils libabigail bash-completion ruby-devel)

	if echo "$ID $VERSION_ID" | grep -E -q 'rocky 8'; then
		devtool_pkgs+=(python3-pycodestyle astyle)
	elif echo "$ID $VERSION_ID" | grep -E -q 'rocky 10'; then
		echo "Rocky 10 do not have python3-pycodestyle and lcov dependencies"
		devtool_pkgs+=(astyle ShellCheck)
	elif [[ $ID == openeuler ]]; then
		devtool_pkgs+=(python3-pycodestyle)
		echo "openEuler does not have astyle, lcov and ShellCheck dependencies"
	else
		devtool_pkgs+=(python-pycodestyle astyle lcov ShellCheck)
	fi

	if [[ $ID == fedora ]]; then
		devtool_pkgs+=(rubygem-{bundler,rake})
	fi

	yum install -y "${devtool_pkgs[@]}"
fi
if [[ $INSTALL_RBD == "true" ]]; then
	# Additional dependencies for RBD bdev in NVMe over Fabrics
	yum install -y librados-devel librbd-devel
fi
if [[ $INSTALL_RDMA == "true" ]]; then
	# Additional dependencies for RDMA transport in NVMe over Fabrics
	yum install -y libibverbs-devel librdmacm-devel
fi
if [[ $INSTALL_DOCS == "true" ]]; then
	# Additional dependencies for building docs
	yum install -y mscgen || echo "Warning: couldn't install mscgen via yum. Please install mscgen manually."
	yum install -y doxygen graphviz
fi
if [[ $INSTALL_DAOS == "true" ]]; then
	if [[ $ID == rocky && $VERSION_ID == 8* ]]; then
		yum install -y daos-devel
	else
		echo "Skipping installation of DAOS bdev dependencies."
		echo "DAOS is supported only under Centos and Rocky (variants 7-8)."
	fi
fi
# Additional dependencies for Avahi
if [[ $INSTALL_AVAHI == "true" ]]; then
	# Additional dependencies for Avahi
	yum install -y avahi-devel
fi
if [[ $INSTALL_IDXD == "true" ]]; then
	yum install -y accel-config-devel
fi
if [[ $INSTALL_LZ4 == "true" ]]; then
	yum install -y lz4-devel
fi
