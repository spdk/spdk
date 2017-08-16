#!/bin/bash

SPDK_DIR=/spdk

SUDOCMD="sudo -H -u vagrant"
echo 0:$0
echo 1:$1
echo 2:$2
echo SUDOCMD: $SUDOCMD

# Figure out what system we are running on
if [ -f /etc/lsb-release ];then
    . /etc/lsb-release
elif [ -f /etc/redhat-release ];then
    sudo yum install -y redhat-lsb
    DISTRIB_ID=`lsb_release -si`
    DISTRIB_RELEASE=`lsb_release -sr`
    DISTRIB_CODENAME=`lsb_release -sc`
    DISTRIB_DESCRIPTION=`lsb_release -sd`
fi
KERNEL_OS=`uname -o`
KERNEL_MACHINE=`uname -m`
KERNEL_RELEASE=`uname -r`
KERNEL_VERSION=`uname -v`

echo KERNEL_OS: $KERNEL_OS
echo KERNEL_MACHINE: $KERNEL_MACHINE
echo KERNEL_RELEASE: $KERNEL_RELEASE
echo KERNEL_VERSION: $KERNEL_VERSION
echo DISTRIB_ID: $DISTRIB_ID
echo DISTRIB_RELEASE: $DISTRIB_RELEASE
echo DISTRIB_CODENAME: $DISTRIB_CODENAME
echo DISTRIB_DESCRIPTION: $DISTRIB_DESCRIPTION

if [ -f /etc/lsb-release ]; then
    apt-get install -y gcc g++ make libcunit1-dev libaio-dev libssl-dev
elif [ -f /etc/redhat-release ]; then
    # Add EPEL repository for CUnit-devel
    yum --enablerepo=extras install -y epel-release
    yum install -y gcc gcc-c++ CUnit-devel libaio-devel openssl-devel
fi

cd $SPDK_DIR
$SUDOCMD ./configure --enable-debug
$SUDOCMD make clean
$SUDOCMD make -j2
