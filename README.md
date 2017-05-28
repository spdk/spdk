Storage Performance Development Kit
===================================

[![Build Status](https://travis-ci.org/spdk/spdk.svg?branch=master)](https://travis-ci.org/spdk/spdk)

Learn all about SPDK at the [spdk.io](http://www.spdk.io) including
documentation, detailed information on the community and how to contribute,
blogs and lots of other useful information. The library includes user space
drivers for NVMe and NVMeOF, an iSCSI target, a block device abstraction
layer and many others! Check out the [spdk.io](http://www.spdk.io) for
a complete list.

Quick Start
===========

* Installing prerequisites, getting/building the code & running unit tests
* Using vagrant to quickly kick the tires in a virtual machine
* Advanced build options
* Use of huge pages
* Information about example code

Install the Prerequisites
=========================

Note: The requirements for building the docs can take a while to
install so you may want to skip them unless you need them.

Fedora/CentOS:

~~~{.sh}
sudo dnf update
sudo dnf install -y gcc gcc-c++ make CUnit-devel libaio-devel openssl-devel \
libibverbs-devel librdmacm-devel git astyle-devel python-pep8 lcov python \
clang-analyzer
# Additional dependencies for building docs
sudo dnf install -y doxygen mscgen
~~~

Ubuntu/Debian:

~~~{.sh}
sudo apt-get update
sudo apt-get install -y gcc g++ make libcunit1-dev libaio-dev libssl-dev \
libibverbs-dev librdmacm-dev git astyle pep8 lcov clang
# Additional dependencies for building docs
sudo apt-get install -y doxygen mscgen
~~~

FreeBSD:

~~~{.sh}
sudo pkg update
sudo pkg install gmake cunit openssl git devel/astyle bash devel/pep8 \
python
# Additional dependencies for building docs
sudo pkg install doxygen mscgen
~~~

Clone the repo and initialize the DPDK submodule
================================================

~~~{.sh}
git clone https://review.gerrithub.io/spdk/spdk
cd spdk
git submodule update --init
~~~

Optionally use the git credential helper to store your GerritHub password
=========================================================================

~~~{.sh}
git config credential.helper store
~~~

Install the commit-msg hook
===========================
This inserts a unique change ID each time you commit and is required.

~~~{.sh}
curl -Lo .git/hooks/commit-msg https://review.gerrithub.io/tools/hooks/commit-msg
chmod +x .git/hooks/commit-msg
~~~

Build the libraries
===================

Linux:

~~~{.sh}
./configure
make
~~~

FreeBSD:
Note: Make sure you have the correct kernel source in /usr/src/ and
also note that CONFIG_COVERAGE option is not available right now
for FreeBSD builds.

~~~{.sh}
./configure
gmake
~~~

Running the unit tests
======================
When you run the tests, you will see lots of expected failure messages,
however seeing the message that all tests pass is what matters.

~~~{.sh}
./unittests.sh
~~~

Using Vagrant
=============

A [Vagrant](https://www.vagrantup.com/downloads.html) setup is also provided
to create a Linux VM with a virtual NVMe controller to get up and running
quickly.  Currently this has only been tested on MacOS and Ubuntu 16.04.2 LTS
with the [VirtualBox](https://www.virtualbox.org/wiki/Downloads) provider.  The
[VirtualBox Extension Pack](https://www.virtualbox.org/wiki/Downloads) must
also be installed in order to get the required NVMe support.

Details on the Vagrant setup can be found in
[scripts/vagrant/README.md](scripts/vagrant/README.md).

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

~~~{.sh}
./configure --with-dpdk=./dpdk/x86_64-native-linuxapp-gcc --with-rdma
~~~

Additionally, `CONFIG` options may also be overrriden on the `make` command
line:

~~~{.sh}
make CONFIG_RDMA=y
~~~

Users may wish to use a version of DPDK different from the submodule included
in the SPDK repository.  To specify an alternate DPDK installation, run
configure with the --with-dpdk option.  For example:

Linux:

~~~{.sh}
./configure --with-dpdk=/path/to/dpdk/x86_64-native-linuxapp-gcc
make
~~~{.sh}

FreeBSD:

~~~{.sh}
./configure --with-dpdk=/path/to/dpdk/x86_64-native-bsdapp-clang
gmake
~~~

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

~~~{.sh}
sudo scripts/setup.sh
~~~

Examples
========

Example code is located in the examples directory. The examples are compiled
automatically as part of the build process. Simply call any of the examples
with no arguments to see the help output. You'll likely need to run the examples
as a privileged user (root) unless you've done additional configuration
to grant your user permission to allocate huge pages and map devices through
vfio.
