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

		*) ;;
	esac
}

is_repo() { yum repolist --all | grep -q "^$1"; }

disclaimer

# First, add extra EPEL, ELRepo, Ceph repos to have a chance of covering most of the packages
# on the enterprise systems, like RHEL.
if [[ $ID == centos || $ID == rhel || $ID == rocky ]]; then
	repos=() enable=("epel" "elrepo" "elrepo-testing")
	[[ $ID == centos || $ID == rocky ]] && enable+=("extras")
	if [[ $VERSION_ID == 7* ]]; then
		repos+=("https://dl.fedoraproject.org/pub/epel/epel-release-latest-7.noarch.rpm")
		repos+=("https://www.elrepo.org/elrepo-release-7.el7.elrepo.noarch.rpm")
		[[ $ID == centos ]] && repos+=("centos-release-ceph-nautilus.noarch")
		[[ $ID == centos ]] && repos+=("centos-release-scl-rh")
		# Disable liburing, see https://github.com/spdk/spdk/issues/1564
		if [[ $INSTALL_LIBURING == true ]]; then
			echo "Liburing not supported on ${ID}$VERSION_ID, disabling"
			INSTALL_LIBURING=false
		fi
	fi
	if [[ $VERSION_ID == 8* ]]; then
		repos+=("https://dl.fedoraproject.org/pub/epel/epel-release-latest-8.noarch.rpm")
		repos+=("https://www.elrepo.org/elrepo-release-8.el8.elrepo.noarch.rpm")
		[[ $ID == centos || $ID == rocky ]] \
			&& repos+=("https://download.ceph.com/rpm-nautilus/el8/noarch/ceph-release-1-1.el8.noarch.rpm")
		# Add PowerTools needed for install CUnit-devel in Centos8
		if [[ $ID == centos || $ID == rocky ]]; then
			is_repo "PowerTools" && enable+=("PowerTools")
			is_repo "powertools" && enable+=("powertools")
		fi
	fi
	if ((${#repos[@]} > 0)); then
		yum install -y "${repos[@]}" yum-utils
		yum-config-manager --enable "${enable[@]}"
	fi
	# Potential dependencies can be needed from other RHEL repos, enable them
	if [[ $ID == rhel ]]; then
		[[ $VERSION_ID == 7* ]] && sub repos --enable "rhel-*-optional-rpms" --enable "rhel-*-extras-rpms"
		[[ $VERSION_ID == 8* ]] && sub repos --enable codeready-builder-for-rhel-8-x86_64-rpms
	fi
fi

yum install -y gcc gcc-c++ make CUnit-devel libaio-devel openssl-devel \
	libuuid-devel libiscsi-devel ncurses-devel json-c-devel libcmocka-devel \
	clang clang-devel python3-pip

# Minimal install
# workaround for arm: ninja fails with dep on skbuild python module
if [ "$(uname -m)" = "aarch64" ]; then
	pip3 install scikit-build
	if echo "$ID $VERSION_ID" | grep -E -q 'centos 7'; then
		# by default centos 7.x uses cmake 2.8 while ninja requires 3.6 or higher
		yum install -y cmake3
		# cmake3 is installed as /usr/bin/cmake3 while ninja directly calls `cmake`. Create a soft link
		# as a workaround
		mkdir -p /tmp/bin/
		ln -s /usr/bin/cmake3 /tmp/bin/cmake > /dev/null 2>&1 || true
		export PATH=/tmp/bin:$PATH
	fi
fi

# for rhel and centos7 OpenSSL 1.1 should be installed via EPEL
if echo "$ID $VERSION_ID" | grep -E -q 'centos 7|rhel 7'; then
	yum install -y openssl11-devel
fi
if echo "$ID $VERSION_ID" | grep -E -q 'centos 8|rhel 8|rocky 8'; then
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
pip3 install ninja
pip3 install meson
pip3 install pyelftools
pip3 install ijson
pip3 install python-magic
if ! [[ $ID == centos && $VERSION_ID == 7 ]]; then
	# Problem with modules compilation on Centos7
	pip3 install grpcio
	pip3 install grpcio-tools
fi
pip3 install pyyaml

# Additional dependencies for SPDK CLI - not available in rhel and centos
if ! echo "$ID $VERSION_ID" | grep -E -q 'rhel 7|centos 7'; then
	yum install -y python3-configshell python3-pexpect
fi
# Additional dependencies for ISA-L used in compression
yum install -y autoconf automake libtool help2man
# Additional dependencies for DPDK
yum install -y numactl-devel nasm
# Additional dependencies for USDT
yum install -y systemtap-sdt-devel
if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
	# Tools for developers
	devtool_pkgs=(git sg3_utils pciutils libabigail bash-completion ruby-devel)

	if echo "$ID $VERSION_ID" | grep -E -q 'centos 8|rocky 8'; then
		devtool_pkgs+=(python3-pycodestyle astyle)
		echo "Centos 8 and Rocky 8 do not have lcov and ShellCheck dependencies"
	elif [[ $ID == openeuler ]]; then
		devtool_pkgs+=(python3-pycodestyle)
		echo "openEuler does not have astyle, lcov and ShellCheck dependencies"
	else
		devtool_pkgs+=(python-pycodestyle astyle lcov ShellCheck)
	fi

	yum install -y "${devtool_pkgs[@]}"
fi
if [[ $INSTALL_PMEM == "true" ]]; then
	# Additional dependencies for building pmem based backends
	yum install -y libpmemblk-devel || true
	yum install -y libpmemobj-devel || true
fi
if [[ $INSTALL_FUSE == "true" ]]; then
	# Additional dependencies for FUSE and NVMe-CUSE
	yum install -y fuse3-devel
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
	if [[ $ID == centos || $ID == rocky ]]; then
		if ! hash yum-config-manager &> /dev/null; then
			yum install -y yum-utils
		fi
		[[ $VERSION_ID == 7* ]] && yum-config-manager --add-repo "https://packages.daos.io/v2.0/CentOS7/packages/x86_64/daos_packages.repo"
		[[ $VERSION_ID == 8* ]] && yum-config-manager --add-repo "https://packages.daos.io/v2.0/EL8/packages/x86_64/daos_packages.repo"
		yum-config-manager --enable "daos-packages"
		yum install -y daos-devel
	else
		echo "Skipping installation of DAOS bdev dependencies. It is supported only for CentOS 7, CentOS 8 and Rocky 8"
	fi
fi
# Additional dependencies for Avahi
if [[ $INSTALL_AVAHI == "true" ]]; then
	# Additional dependencies for Avahi
	yum install -y avahi-devel
fi
