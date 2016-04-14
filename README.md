Storage Performance Development Kit
===================================

[![Build Status](https://travis-ci.org/spdk/spdk.svg?branch=master)](https://travis-ci.org/spdk/spdk)
[![Gitter](https://img.shields.io/gitter/room/spdk/spdk.svg)](https://gitter.im/spdk/spdk)

[SPDK Mailing List](https://lists.01.org/mailman/listinfo/spdk)

[SPDK on 01.org](https://01.org/spdk)

The Storage Performance Development Kit (SPDK) provides a set of tools
and libraries for writing high performance, scalable, user-mode storage
applications. It achieves high performance by moving all of the necessary
drivers into userspace and operating in a polled mode instead of relying on
interrupts, which avoids kernel context switches and eliminates interrupt
handling overhead.

The development kit currently includes:
* NVMe driver
* I/OAT (DMA engine) driver

Documentation
=============

[Doxygen API documentation](https://spdk.github.io/spdk/doc/) is available, as
well as a [Porting Guide](PORTING.md) for porting SPDK to different frameworks
and operating systems.

Many examples are available in the `examples` directory.

[Changelog](CHANGELOG.md)

Prerequisites
=============

To build SPDK, some dependencies must be installed.

Fedora/CentOS:

    sudo dnf install -y gcc libpciaccess-devel CUnit-devel libaio-devel

Ubuntu/Debian:

    sudo apt-get install -y gcc libpciaccess-dev make libcunit1-dev libaio-dev

FreeBSD:

- gcc
- libpciaccess
- gmake
- cunit

Additionally, [DPDK](http://dpdk.org/doc/quick-start) is required.

    1) cd /path/to/spdk
    2) wget http://dpdk.org/browse/dpdk/snapshot/dpdk-2.2.0.tar.gz
    3) tar xfz dpdk-2.2.0.tar.gz

Linux:

    4) (cd dpdk-2.2.0 && make install T=x86_64-native-linuxapp-gcc DESTDIR=.)

FreeBSD:

    4) (cd dpdk-2.2.0 && gmake install T=x86_64-native-bsdapp-clang DESTDIR=.)

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
SPDK includes a script to automate this process on both Linux and FreeBSD.
This script should be run as root.

    sudo scripts/setup.sh

Examples
========

Example code is located in the examples directory. The examples are compiled
automatically as part of the build process. Simply call any of the examples
with no arguments to see the help output. You'll likely need to run the examples
as a privileged user (root) unless you've done additional configuration
to grant your user permission to allocate huge pages and map devices through
vfio.
