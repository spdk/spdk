#!/usr/bin/env bash
# Please run this script as root.

set -e
trap 'set +e; trap - ERR; echo "Error!"; exit 1;' ERR

scriptsdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $scriptsdir/..)

if [ -s /etc/redhat-release ]; then
	. /etc/os-release

	# Includes Fedora, CentOS 7, RHEL 7
	# Add EPEL repository for CUnit-devel and libunwind-devel
	if echo "$ID $VERSION_ID" | egrep -q 'rhel 7|centos 7'; then
		if ! rpm --quiet -q epel-release; then
			yum install https://dl.fedoraproject.org/pub/epel/epel-release-latest-7.noarch.rpm
		fi

		if [ $ID = 'rhel' ]; then
			subscription-manager repos --enable "rhel-*-optional-rpms" --enable "rhel-*-extras-rpms"
		elif [ $ID = 'centos' ]; then
			yum --enablerepo=extras install -y epel-release
		fi
	fi

	yum install -y gcc gcc-c++ make CUnit-devel libaio-devel openssl-devel \
		git astyle python-pep8 lcov python clang-analyzer libuuid-devel \
		sg3_utils libiscsi-devel pciutils
	# Additional (optional) dependencies for showing backtrace in logs
	yum install -y libunwind-devel || true
	# Additional dependencies for NVMe over Fabrics
	yum install -y libibverbs-devel librdmacm-devel
	# Additional dependencies for DPDK
	yum install -y numactl-devel nasm
	# Additional dependencies for building docs
	yum install -y doxygen mscgen graphviz
	# Additional dependencies for building pmem based backends
	yum install -y libpmemblk-devel || true
	# Additional dependencies for SPDK CLI - not available in rhel and centos
	if ! echo "$ID $VERSION_ID" | egrep -q 'rhel 7|centos 7'; then
		yum install -y python3-configshell python3-pexpect
	fi
elif [ -f /etc/debian_version ]; then
	# Includes Ubuntu, Debian
	apt-get install -y gcc g++ make libcunit1-dev libaio-dev libssl-dev \
		git astyle pep8 lcov clang uuid-dev sg3-utils libiscsi-dev pciutils
	# Additional (optional) dependencies for showing backtrace in logs
	apt-get install -y libunwind-dev || true
	# Additional dependencies for NVMe over Fabrics
	apt-get install -y libibverbs-dev librdmacm-dev
	# Additional dependencies for DPDK
	apt-get install -y libnuma-dev nasm
	# Additional dependencies for building docs
	apt-get install -y doxygen mscgen graphviz
	# Additional dependencies for SPDK CLI
	apt-get install -y python-pip python3-pip
	pip install configshell_fb pexpect
	pip3 install configshell_fb pexpect
elif [ -f /etc/SuSE-release ]; then
	zypper install -y gcc gcc-c++ make cunit-devel libaio-devel libopenssl-devel \
		git-core lcov python-base python-pep8 libuuid-devel sg3_utils pciutils
	# Additional (optional) dependencies for showing backtrace in logs
	zypper install libunwind-devel || true
	# Additional dependencies for NVMe over Fabrics
	zypper install -y rdma-core-devel
	# Additional dependencies for DPDK
	zypper install -y libnuma-devel nasm
	# Additional dependencies for building pmem based backends
	zypper install -y libpmemblk-devel
	# Additional dependencies for building docs
	zypper install -y doxygen mscgen graphviz
elif [ $(uname -s) = "FreeBSD" ] ; then
	pkg install -y gmake cunit openssl git devel/astyle bash py27-pycodestyle \
		python misc/e2fsprogs-libuuid sysutils/sg3_utils nasm
	# Additional dependencies for building docs
	pkg install -y doxygen mscgen graphviz
else
	echo "pkgdep: unknown system type."
	exit 1
fi

# Only crypto needs nasm and this lib but because the lib requires root to
# install we do it here.
nasm_ver=$(nasm -v | sed 's/[^0-9]*//g' | awk '{print substr ($0, 0, 5)}')
if [ $nasm_ver -lt "21202" ]; then
		echo Crypto requires NASM version 2.12.02 or newer.  Please install
		echo or upgrade and re-run this script if you are going to use Crypto.
else
	ipsec="$(find /usr -name intel-ipsec-mb.h 2>/dev/null)"
	if [ "$ipsec" == "" ]; then
		ipsec_submodule_cloned="$(find $rootdir/intel-ipsec-mb -name intel-ipsec-mb.h 2>/dev/null)"
		if [ "$ipsec_submodule_cloned" != "" ]; then
			su - $SUDO_USER -c "make -C $rootdir/intel-ipsec-mb"
			make -C $rootdir/intel-ipsec-mb install
		else
			echo "The intel-ipsec-mb submodule has not been cloned and will not be installed."
			echo "To enable crypto, run 'git submodule update --init' and then run this script again."
		fi
	fi
fi
