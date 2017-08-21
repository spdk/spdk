#!/usr/bin/env bash

PLATFORM=`/bin/uname`
case $PLATFORM in
  HP-UX)
    OS=HP-UX ;;
  AIX)
    OS=AIX ;;
  SunOS)
    OS=SunOS ;;
  Linux)
    if [ -f /etc/centos-release ]; then
        OS=CentOS
    elif [ -s /etc/redhat-release ]; then
        OS=RedHat
    elif [ -r /etc/os-release ]; then
        grep 'NAME="Ubuntu"' /etc/os-release > /dev/null 2>&1
        if [ $? == 0 ]; then
            OS=Ubuntu
        fi
        grep 'NAME="Debian GNU/Linux"' /etc/os-release > /dev/null 2>&1
        if [ $? == 0 ]; then
            OS=Debian
        fi
        grep 'NAME="FreeBSD' /etc/os-release > /dev/null 2>&1
        if [ $? == 0 ]; then
            OS=Freebsd
        fi
    else
        OS="Unknown Linux"
    fi ;;
  *)
    OS="Unknown UNIX/Linux" ;;
esac
echo $OS

if [ "$OS" = RedHat ] || [ "$OS" = CentOS ]; then
	sudo dnf install -y gcc gcc-c++ make CUnit-devel libaio-devel openssl-devel \
		git astyle-devel python-pep8 lcov python clang-analyzer
	# Additional dependencies for NVMe over Fabrics
	sudo dnf install -y libibverbs-devel librdmacm-devel
	# Additional dependencies for building docs
	sudo dnf install -y doxygen mscgen
fi

if [ "$OS" = Ubuntu ] || [ "$OS" = Debian ]; then
	sudo apt-get install -y gcc g++ make libcunit1-dev libaio-dev libssl-dev \
		git astyle pep8 lcov clang
	# Additional dependencies for NVMe over Fabrics
	sudo apt-get install -y libibverbs-dev librdmacm
	# Additional dependencies for building docs
	sudo apt-get install -y doxygen mscgen
fi

if [ "$OS" = FreeBSD ]; then
	sudo pkg install gmake cunit openssl git devel/astyle bash devel/pep8 \
		python
	# Additional dependencies for building docs
	sudo pkg install doxygen mscgen
fi
