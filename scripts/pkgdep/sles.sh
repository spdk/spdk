#!/usr/bin/env bash

# Minimal install
zypper install -y gcc gcc-c++ make cmake cunit-devel libaio-devel libopenssl-devel \
	libuuid-devel python-base ncurses-devel ninja meson
# Additional dependencies for DPDK
zypper install -y libnuma-devel nasm
# Additional dependencies for ISA-L used in compression
zypper install -y autoconf automake libtool help2man
if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
	# Tools for developers
	zypper install -y git-core lcov python-pycodestyle sg3_utils \
		pciutils ShellCheck
fi
if [[ $INSTALL_PMEM == "true" ]]; then
	# Additional dependencies for building pmem based backends
	zypper install -y libpmemblk-devel
fi
if [[ $INSTALL_FUSE == "true" ]]; then
	# Additional dependencies for FUSE and NVMe-CUSE
	zypper install -y fuse3-devel
fi
if [[ $INSTALL_RDMA == "true" ]]; then
	# Additional dependencies for RDMA transport in NVMe over Fabrics
	zypper install -y rdma-core-devel
fi
if [[ $INSTALL_DOCS == "true" ]]; then
	# Additional dependencies for building docs
	zypper install -y doxygen mscgen graphviz
fi
