#!/usr/bin/env bash

# Minimal install
if echo "$ID $VERSION_ID" | grep -E -q 'centos 8'; then
	# Add PowerTools needed for install CUnit-devel in Centos8
	yum install -y yum-utils
	yum config-manager --set-enabled PowerTools
fi
yum install -y gcc gcc-c++ make CUnit-devel libaio-devel openssl-devel \
	libuuid-devel libiscsi-devel ncurses-devel
if echo "$ID $VERSION_ID" | grep -E -q 'centos 8'; then
	yum install -y python36
	#Create hard link to use in SPDK as python
	ln /etc/alternatives/python3 /usr/bin/python || true
else
	yum install -y python
fi
yum install -y python3-pip
pip-3 install ninja
pip-3 install meson

# Additional dependencies for SPDK CLI - not available in rhel and centos
if ! echo "$ID $VERSION_ID" | grep -E -q 'rhel 7|centos 7'; then
	yum install -y python3-configshell python3-pexpect
fi
# Additional dependencies for ISA-L used in compression
yum install -y autoconf automake libtool help2man
# Additional dependencies for DPDK
yum install -y numactl-devel nasm
if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
	# Tools for developers
	# Includes Fedora, CentOS 7, RHEL 7
	# Add EPEL repository for CUnit-devel
	if echo "$ID $VERSION_ID" | grep -E -q 'rhel 7|centos 7|centos 8'; then
		if ! rpm --quiet -q epel-release; then
			yum install -y https://dl.fedoraproject.org/pub/epel/epel-release-latest-7.noarch.rpm
		fi

		if [[ $ID = 'rhel' ]]; then
			subscription-manager repos --enable "rhel-*-optional-rpms" --enable "rhel-*-extras-rpms"
		elif [[ $ID = 'centos' ]]; then
			yum --enablerepo=extras install -y epel-release
		fi
	fi
	if echo "$ID $VERSION_ID" | grep -E -q 'centos 8'; then
		yum install -y python3-pycodestyle
		echo "Centos 8 does not have lcov and ShellCheck dependencies"
	else
		yum install -y python-pycodestyle lcov ShellCheck
	fi
	yum install -y git astyle sg3_utils pciutils
	install_shfmt
fi
if [[ $INSTALL_PMEM == "true" ]]; then
	# Additional dependencies for building pmem based backends
	yum install -y libpmemblk-devel || true
fi
if [[ $INSTALL_FUSE == "true" ]]; then
	# Additional dependencies for FUSE and NVMe-CUSE
	yum install -y fuse3-devel
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
if [[ $INSTALL_LIBURING == "true" ]]; then
	install_liburing
fi
