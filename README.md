Storage Performance Development Kit
===================================

[SPDK on 01.org](https://01.org/spdk)

The Storage Performance Development Kit (SPDK) provides a set of tools
and libraries for writing high performance, scalable, user-mode storage
applications.
It achieves high performance by moving all of the necessary drivers into
userspace and operating in a polled mode instead of relying on interrupts,
which avoids kernel context switches and eliminates interrupt handling
overhead.

Prerequisites
=============

To build SPDK, some dependencies must be installed.

Fedora/CentOS:

- gcc
- libpciaccess-devel
- CUnit-devel

Ubuntu/Debian:

- gcc
- libpciaccess-dev
- make
- libcunit1-dev

Additionally, [DPDK](http://dpdk.org/doc/quick-start) is required.

    1) cd /path/to/spdk
    2) wget http://dpdk.org/browse/dpdk/snapshot/dpdk-2.1.0.tar.gz
    3) tar xfz dpdk-2.1.0.tar.gz
    4) cd dpdk-2.1.0
    5) make install T=x86_64-native-linuxapp-gcc

Building
========

Once the prerequisites are installed, run 'make' within the SPDK directory
to build the SPDK libraries and examples.

    make DPDK_DIR=/path/to/dpdk

If you followed the instructions above for building DPDK:

    make DPDK_DIR=`pwd`/dpdk-2.1.0/x86_64-native-linuxapp-gcc
