#!/bin/sh
# Please run this script as root.

SYSTEM=`uname -s`

if [ -s /etc/redhat-release ]; then
	# Includes Fedora, CentOS
	if [ -f /etc/centos-release ]; then
		# Add EPEL repository for CUnit-devel
		yum --enablerepo=extras install -y epel-release
	fi
	yum install -y gcc gcc-c++ make CUnit-devel libaio-devel openssl-devel \
		git astyle-devel python-pep8 lcov python clang-analyzer libuuid-devel \
		sg3_utils libiscsi-devel
	# Additional dependencies for NVMe over Fabrics
	yum install -y libibverbs-devel librdmacm-devel
	# Additional dependencies for DPDK
	yum install -y numactl-devel
	# Additional dependencies for building docs
	yum install -y doxygen mscgen graphviz
	# Additional dependencies for building pmem based backends
	yum install -y libpmemblk-devel || true
	# Additional dependencies for SPDK CLI
	yum install -y python-configshell
elif [ -f /etc/debian_version ]; then
	# Includes Ubuntu, Debian
	apt-get install -y gcc g++ make libcunit1-dev libaio-dev libssl-dev \
		git astyle pep8 lcov clang uuid-dev sg3-utils libiscsi-dev
	# Additional dependencies for NVMe over Fabrics
	apt-get install -y libibverbs-dev librdmacm-dev
	# Additional dependencies for DPDK
	apt-get install -y libnuma-dev
	# Additional dependencies for building docs
	apt-get install -y doxygen mscgen graphviz
	# Additional dependencies for SPDK CLI
	apt-get install -y "python-configshell*"
elif [ $SYSTEM = "FreeBSD" ] ; then
	pkg install gmake cunit openssl git devel/astyle bash devel/pep8 \
		python misc/e2fsprogs-libuuid sysutils/sg3_utils
	# Additional dependencies for building docs
	pkg install doxygen mscgen graphviz
elif [[ `uname -a` = *"Linux clr"* ]]; then
	swupd bundle-add c-basic # gcc, make, bison, flex,
	swupd bundle-add storage-utils-dev # CUnit-dev, libaio-dev, openssl-dev, git, python, numactl-dev, doxygen, graphviz
	swupd bundle-add user-basic-dev # lcov
	swupd bundle-add os-clr-on-clr # pep8
	swupd bundle-add user-basic-dev # libiscsi-devel
	swupd bundle-add hpc-utils # rdma-core


	# astyle-devel, clang-analyzer, libuuid-devel, sg3_utils, mscgen, libpmemblk-devel, python-configshell
else
	echo "pkgdep: unknown system type."
	exit 1
fi
