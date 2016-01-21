Storage Performance Development Kit
===================================

[![Build Status](https://travis-ci.org/spdk/spdk.svg?branch=master)](https://travis-ci.org/spdk/spdk)

[SPDK on 01.org](https://01.org/spdk)

The Storage Performance Development Kit (SPDK) provides a set of tools
and libraries for writing high performance, scalable, user-mode storage
applications.
It achieves high performance by moving all of the necessary drivers into
userspace and operating in a polled mode instead of relying on interrupts,
which avoids kernel context switches and eliminates interrupt handling
overhead.

Documentation
=============

[Doxygen API documentation](https://spdk.github.io/spdk/doc/)

[Porting Guide](PORTING.md)

Prerequisites
=============

To build SPDK, some dependencies must be installed.

Fedora/CentOS:

- gcc
- libpciaccess-devel
- CUnit-devel
- libaio-devel

Ubuntu/Debian:

- gcc
- libpciaccess-dev
- make
- libcunit1-dev
- libaio-dev

FreeBSD:

- gcc
- libpciaccess
- gmake
- cunit

Additionally, [DPDK](http://dpdk.org/doc/quick-start) is required.

    1) cd /path/to/spdk
    2) wget http://dpdk.org/browse/dpdk/snapshot/dpdk-2.2.0.tar.gz
    3) tar xfz dpdk-2.2.0.tar.gz
    4) cd dpdk-2.2.0

Linux:

    5) make install T=x86_64-native-linuxapp-gcc DESTDIR=.

FreeBSD:

    5) gmake install T=x86_64-native-bsdapp-clang DESTDIR=.

Building
========

Once the prerequisites are installed, run 'make' within the SPDK directory
to build the SPDK libraries and examples.

    make DPDK_DIR=/path/to/dpdk

If you followed the instructions above for building DPDK:

Linux:

    make DPDK_DIR=./dpdk-2.2.0/x86_64-native-linuxapp-gcc

FreeBSD:

    gmake DPDK_DIR=./dpdk-2.2.0/x86_64-native-bsdapp-clang

Hugepages and Device Binding
============================

Before running an SPDK application, some hugepages must be allocated and
any NVMe and I/OAT devices must be unbound from the native kernel drivers.
SPDK includes scripts to automate this process on both Linux and FreeBSD.

    1) scripts/configure_hugepages.sh
    2) scripts/unbind.sh
