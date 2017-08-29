# Storage Performance Development Kit

[![Build Status](https://travis-ci.org/spdk/spdk.svg?branch=master)](https://travis-ci.org/spdk/spdk)

The Storage Performance Development Kit ([SPDK](http://www.spdk.io)) provides a set of tools
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

# In this readme:

* [Documentation](#documentation)
* [Prerequisites](#prerequisites)
* [Source Code](#source)
* [Build](#libraries)
* [Unit Tests](#tests)
* [Vagrant](#vagrant)
* [Advanced Build Options](#advanced)
* [Hugepages and Device Binding](#huge)
* [Example Code](#examples)
* [Contributing](#contributing)

<a id="documentation"></a>
## Documentation

[Doxygen API documentation](http://www.spdk.io/doc/) is available, as
well as a [Porting Guide](http://www.spdk.io/doc/porting.html) for porting SPDK to different frameworks
and operating systems.

<a id="prerequisites"></a>
## Prerequisites

Note: The requirements for building the docs can take a while to
install so you may want to skip them unless you need them.

Fedora/CentOS:

~~~{.sh}
sudo dnf install -y gcc gcc-c++ make CUnit-devel libaio-devel openssl-devel \
	git astyle-devel python-pep8 lcov python clang-analyzer
# Additional dependencies for RDMA (NVMe over Fabrics)
sudo dnf install -y libibverbs-devel librdmacm-devel
# Additional dependencies for building docs
sudo dnf install -y doxygen mscgen
~~~

Ubuntu/Debian:

~~~{.sh}
sudo apt-get install -y gcc g++ make libcunit1-dev libaio-dev libssl-dev \
	git astyle pep8 lcov clang
# Additional dependencies for RDMA (NVMe over Fabrics)
sudo apt-get install -y libibverbs-dev librdmacm
# Additional dependencies for building docs
sudo apt-get install -y doxygen mscgen
~~~

FreeBSD:

~~~{.sh}
sudo pkg install gmake cunit openssl git devel/astyle bash devel/pep8 \
	python
# Additional dependencies for building docs
sudo pkg install doxygen mscgen
~~~

<a id="source"></a>
## Source Code

~~~{.sh}
git clone https://github.com/spdk/spdk
cd spdk
git submodule update --init
~~~

<a id="libraries"></a>
## Build

Linux:

~~~{.sh}
./configure
make
~~~

FreeBSD:
Note: Make sure you have the matching kernel source in /usr/src/ and
also note that CONFIG_COVERAGE option is not available right now
for FreeBSD builds.

~~~{.sh}
./configure
gmake
~~~

<a id="tests"></a>
## Unit Tests

~~~{.sh}
./unittest.sh
~~~

You will see several error messages when running the unit tests, but they are
part of the test suite. The final message at the end of the script indicates
success or failure.

<a id="vagrant"></a>
## Vagrant

A [Vagrant](https://www.vagrantup.com/downloads.html) setup is also provided
to create a Linux VM with a virtual NVMe controller to get up and running
quickly.  Currently this has only been tested on MacOS and Ubuntu 16.04.2 LTS
with the [VirtualBox](https://www.virtualbox.org/wiki/Downloads) provider.  The
[VirtualBox Extension Pack](https://www.virtualbox.org/wiki/Downloads) must
also be installed in order to get the required NVMe support.

Details on the Vagrant setup can be found in the
[SPDK Vagrant documentation](http://spdk.io/doc/vagrant.html).

<a id="advanced"></a>
## Advanced Build Options

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
./configure --with-rdma
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
~~~

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

<a id="huge"></a>
## Hugepages and Device Binding

Before running an SPDK application, some hugepages must be allocated and
any NVMe and I/OAT devices must be unbound from the native kernel drivers.
SPDK includes a script to automate this process on both Linux and FreeBSD.
This script should be run as root.

~~~{.sh}
sudo scripts/setup.sh
~~~

<a id="examples"></a>
## Example Code

Example code is located in the examples directory. The examples are compiled
automatically as part of the build process. Simply call any of the examples
with no arguments to see the help output. You'll likely need to run the examples
as a privileged user (root) unless you've done additional configuration
to grant your user permission to allocate huge pages and map devices through
vfio.

<a id="contributing"></a>
## Contributing

For additional details on how to get more involved in the community, including
[contributing code](http://www.spdk.io/development) and participating in discussions and other activiites, please
refer to [spdk.io](http://www.spdk.io/community)
