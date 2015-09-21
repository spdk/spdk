Storage Performance Development Kit
===================================

[https://01.org/spdk](SPDK on 01.org)

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

Additionally, DPDK is required.
See [http://dpdk.org/doc/quick-start](DPDK Quick Start).

Building
========

Once the prerequisites are installed, run 'make' within the SPDK directory
to build the SPDK libraries and examples.

    make DPDK_DIR=/path/to/dpdk
