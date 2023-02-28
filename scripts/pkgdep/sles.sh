#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#
# Minimal install
zypper install -y gcc gcc-c++ make cunit-devel libaio-devel libopenssl-devel \
	libuuid-devel python-base ncurses-devel libjson-c-devel libcmocka-devel \
	ninja meson python3-pyelftools
# Additional dependencies for DPDK
zypper install -y libnuma-devel nasm
# Additional dependencies for ISA-L used in compression
zypper install -y autoconf automake libtool help2man
if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
	# Tools for developers
	zypper install -y git-core lcov python3-pycodestyle sg3_utils \
		pciutils ShellCheck bash-completion
fi
if [[ $INSTALL_PMEM == "true" ]]; then
	# Additional dependencies for building pmem based backends
	zypper install -y libpmemblk-devel
	zypper install -y libpmemobj-devel
fi
if [[ $INSTALL_FUSE == "true" ]]; then
	# Additional dependencies for FUSE and NVMe-CUSE
	zypper install -y fuse3-devel
fi
if [[ $INSTALL_RBD == "true" ]]; then
	# Additional dependencies for RBD bdev in NVMe over Fabrics
	zypper install -y librados-devel librbd-devel
fi
if [[ $INSTALL_RDMA == "true" ]]; then
	# Additional dependencies for RDMA transport in NVMe over Fabrics
	zypper install -y rdma-core-devel
fi
if [[ $INSTALL_DOCS == "true" ]]; then
	# Additional dependencies for building docs
	zypper install -y doxygen mscgen graphviz
fi
if [[ $INSTALL_DAOS == "true" ]]; then
	zypper ar https://packages.daos.io/v2.0/Leap15/packages/x86_64/ daos_packages
	rpm --import https://packages.daos.io/RPM-GPG-KEY
	zypper --non-interactive refresh
	zypper install -y daos-client daos-devel
fi
if [[ $INSTALL_AVAHI == "true" ]]; then
	# Additional dependencies for Avahi
	zypper install -y avahi-devel
fi
