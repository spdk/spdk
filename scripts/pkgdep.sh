#!/usr/bin/env bash
# Please run this script as root.

set -e

function usage()
{
	echo ""
	echo "This script is intended to automate the installation of package dependencies to build SPDK."
	echo "Please run this script as root user."
	echo ""
	echo "$0"
	echo "  -h --help"
	echo ""
	exit 0
}

INSTALL_CRYPTO=false

while getopts 'hi-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage;;
			*) echo "Invalid argument '$OPTARG'"
			usage;;
		esac
		;;
	h) usage;;
	*) echo "Invalid argument '$OPTARG'"
	usage;;
	esac
done

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
		git astyle python-pycodestyle lcov python clang-analyzer libuuid-devel \
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
	# Additional dependencies for ISA-L used in compression
	yum install -y autoconf automake libtool help2man
elif [ -f /etc/debian_version ]; then
	# Includes Ubuntu, Debian
	apt-get install -y gcc g++ make libcunit1-dev libaio-dev libssl-dev \
		git astyle pep8 lcov clang uuid-dev sg3-utils libiscsi-dev pciutils
	# Additional python style checker not available on ubuntu 16.04 or earlier.
	apt-get install -y pycodestyle || true
	# Additional (optional) dependencies for showing backtrace in logs
	apt-get install -y libunwind-dev || true
	# Additional dependencies for NVMe over Fabrics
	apt-get install -y libibverbs-dev librdmacm-dev
	# Additional dependencies for DPDK
	apt-get install -y libnuma-dev nasm
	# Additional dependencies for building docs
	apt-get install -y doxygen mscgen graphviz
	# Additional dependencies for SPDK CLI - not available on older Ubuntus
	apt-get install -y python3-configshell-fb python3-pexpect || echo \
		"Note: Some SPDK CLI dependencies could not be installed."
	# Additional dependencies for ISA-L used in compression
	apt-get install -y autoconf automake libtool help2man
elif [ -f /etc/SuSE-release ] || [ -f /etc/SUSE-brand ]; then
	zypper install -y gcc gcc-c++ make cunit-devel libaio-devel libopenssl-devel \
		git-core lcov python-base python-pycodestyle libuuid-devel sg3_utils pciutils
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
	# Additional dependencies for ISA-L used in compression
	zypper install -y autoconf automake libtool help2man
elif [ $(uname -s) = "FreeBSD" ] ; then
	pkg install -y gmake cunit openssl git devel/astyle bash py27-pycodestyle \
		python misc/e2fsprogs-libuuid sysutils/sg3_utils nasm
	# Additional dependencies for building docs
	pkg install -y doxygen mscgen graphviz
	# Additional dependencies for ISA-L used in compression
	pkg install -y autoconf automake libtool help2man
else
	echo "pkgdep: unknown system type."
	exit 1
fi
