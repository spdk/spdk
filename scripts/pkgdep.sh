#!/bin/sh
#Please run this script as root.

SYSTEM=`uname -s`

if [ -s /etc/redhat-release ]; then
	# Includes Fedora, CentOS
	yum install -y gcc gcc-c++ make CUnit-devel libaio-devel openssl-devel \
		git astyle-devel python-pep8 lcov python clang-analyzer
	# Additional dependencies for NVMe over Fabrics
	yum install -y libibverbs-devel librdmacm-devel
	# Additional dependencies for building docs
	yum install -y doxygen mscgen
elif [ -f /etc/lsb-release ] || [ -f /etc/debian_version ]; then
	# Includes Ubuntu, Debian
	apt-get install -y gcc g++ make libcunit1-dev libaio-dev libssl-dev \
		git astyle pep8 lcov clang
	# Additional dependencies for NVMe over Fabrics
	apt-get install -y libibverbs-dev librdmacm
	# Additional dependencies for building docs
	apt-get install -y doxygen mscgen
elif [ $SYSTEM = "FreeBSD" ] ; then
	pkg install gmake cunit openssl git devel/astyle bash devel/pep8 \
		python
	# Additional dependencies for building docs
	pkg install doxygen mscgen
else
	echo  "Unknown Linux"
fi
