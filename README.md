Storage Performance Development Kit
===================================

[![Build Status](https://travis-ci.org/spdk/spdk.svg?branch=master)](https://travis-ci.org/spdk/spdk)

[SPDK Mailing List](https://lists.01.org/mailman/listinfo/spdk)

[SPDK on 01.org](https://01.org/spdk)

The Storage Performance Development Kit (SPDK) provides a set of tools
and libraries for writing high performance, scalable, user-mode storage
applications. It achieves high performance by moving all of the necessary
drivers into userspace and operating in a polled mode instead of relying on
interrupts, which avoids kernel context switches and eliminates interrupt
handling overhead.

The development kit currently includes:
* [NVMe driver](http://www.spdk.io/doc/nvme.html)
* [I/OAT (DMA engine) driver](http://www.spdk.io/doc/ioat.html)
* [NVMe over Fabrics target](http://www.spdk.io/doc/nvmf.html)
* [iSCSI target](http://www.spdk.io/doc/iscsi.html)
* [vhost target](http://www.spdk.io/doc/vhost.html)

Documentation
=============

[Doxygen API documentation](http://www.spdk.io/doc/) is available, as
well as a [Porting Guide](http://www.spdk.io/doc/porting.html) for porting SPDK to different frameworks
and operating systems.

Many examples are available in the `examples` directory.

[Changelog](CHANGELOG.md)

Prerequisites
=============

To build SPDK, some dependencies must be installed.

Fedora/CentOS:

    sudo dnf install -y gcc gcc-c++ CUnit-devel libaio-devel openssl-devel
    # Additional dependencies for NVMe over Fabrics:
    sudo dnf install -y libibverbs-devel librdmacm-devel

Ubuntu/Debian:

    sudo apt-get install -y gcc g++ make libcunit1-dev libaio-dev libssl-dev
    # Additional dependencies for NVMe over Fabrics:
    sudo apt-get install -y libibverbs-dev librdmacm-dev

FreeBSD:

    sudo pkg install gmake cunit openssl

Additionally, [DPDK](http://dpdk.org/doc/quick-start) is required.

    1) cd /path/to/spdk
    2) wget http://fast.dpdk.org/rel/dpdk-17.02.tar.xz
    3) tar xf dpdk-17.02.tar.xz

Linux:

    4) (cd dpdk-17.02 && make install T=x86_64-native-linuxapp-gcc DESTDIR=.)

FreeBSD:

    4) (cd dpdk-17.02 && gmake install T=x86_64-native-bsdapp-clang DESTDIR=.)

Building
========

Once the prerequisites are installed, building follows the common configure
and make pattern. If you followed the instructions above for building DPDK:

Linux:

    ./configure --with-dpdk=./dpdk-17.02/x86_64-native-linuxapp-gcc
    make

FreeBSD:

    ./configure --with-dpdk=./dpdk-17.02/x86_64-native-bsdapp-clang
    gmake

Advanced Build Options
======================

Optional components and other build-time configuration are controlled by
settings in two Makefile fragments in the root of the repository. `CONFIG`
contains the base settings. Running the `configure` script generates a new
file, `CONFIG.local`, that contains overrides to the base `CONFIG` file. For
advanced configuration, there are a number of additional options to `configure`
that may be used, or `CONFIG.local` can simply be created and edited by hand. A
description of all possible options is located in `CONFIG`.

Boolean (on/off) options are configured with a 'y' (yes) or 'n' (no). For
example, this line of `CONFIG` controls whether the optional RDMA (libibverbs)
support is enabled:

    CONFIG_RDMA?=n

To enable RDMA, this line may be added to `CONFIG.local` with a 'y' instead of
'n'. For the majority of options this can be done using the `configure` script.
For example:

    ./configure --with-dpdk=./dpdk-17.02/x86_64-native-linuxapp-gcc --with-rdma

Additionally, `CONFIG` options may also be overrriden on the `make` command
line:

    make CONFIG_RDMA=y

The options specified on the `make` command line take precedence over the
default values in `CONFIG` and `CONFIG.local`. This can be useful if you, for
example, generate a `CONFIG.local` using the `configure` script and then have
one or two options (i.e. debug builds) that you wish to turn on and off
frequently.

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
