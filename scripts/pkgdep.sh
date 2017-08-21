#!/usr/bin/env bash
#If FreeBSD, please transfer csh to bash, or use "#!/usr/bin/env sh"
#Run this script as root.

if [ -s /etc/redhat-release ]; then
	# Includes Fedora, CentOS
	dnf install -y gcc gcc-c++ make CUnit-devel libaio-devel openssl-devel \
		git astyle-devel python-pep8 lcov python clang-analyzer
	# Additional dependencies for NVMe over Fabrics
	dnf install -y libibverbs-devel librdmacm-devel
	# Additional dependencies for building docs
	dnf install -y doxygen mscgen
fi

if [ -f /etc/lsb-release ]; then
	# Includes Ubuntu, Debian
	apt-get install -y gcc g++ make libcunit1-dev libaio-dev libssl-dev \
		git astyle pep8 lcov clang
	# Additional dependencies for NVMe over Fabrics
	apt-get install -y libibverbs-dev librdmacm
	# Additional dependencies for building docs
	apt-get install -y doxygen mscgen
fi

SYSTEM=`uname -s`
if [ $SYSTEM = "FreeBSD" ] ; then
	pkg install gmake cunit openssl git devel/astyle bash devel/pep8 \
		python
	# Additional dependencies for building docs
	pkg install doxygen mscgen
fi
