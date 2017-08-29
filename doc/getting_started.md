# Getting Started {#getting_started}

# Installing Prerequisites {#getting_started_prerequisites}

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

# Getting the Source Code {#getting_started_source}

~~~{.sh}
git clone https://github.com/spdk/spdk
cd spdk
git submodule update --init
~~~

# Building {#getting_started_building}

Linux:

~~~{.sh}
./configure
make
~~~

FreeBSD:
Note: Make sure you have the matching kernel source in /usr/src/

~~~{.sh}
./configure
gmake
~~~

There are a number of options available for the configure script, which can
be viewed by running

~~~{.sh}
./configure --help
~~~

Note that not all features are enabled by default. For example, RDMA
support (and hence NVMe over Fabrics) is not enabled by default. You
can enable it by doing the following:

~~~{.sh}
./configure --with-rdma
make
~~~

# Running the Unit Tests {#getting_started_unittests}

It's always a good idea to confirm your build worked by running the
unit tests.

~~~{.sh}
./unittest.sh
~~~

You will see several error messages when running the unit tests, but they are
part of the test suite. The final message at the end of the script indicates
success or failure.

# Running the Example Applications {#getting_started_examples}

Before running an SPDK application, some hugepages must be allocated and
any NVMe and I/OAT devices must be unbound from the native kernel drivers.
SPDK includes a script to automate this process on both Linux and FreeBSD.
This script should be run as root. It only needs to be run once on the
system. *Make sure you aren't using an NVMe device as your boot device.*

~~~{.sh}
sudo scripts/setup.sh
~~~

To rebind devices back to the kernel, you can run

~~~{.sh}
sudo scripts/setup.sh reset
~~~

By default, the script allocates 1024 2MB hugepages. To change this number,
specify NRHUGE as follows:

~~~{.sh}
sudo NRHUGE=4096 scripts/setup.sh
~~~

Example code is located in the examples directory. The examples are compiled
automatically as part of the build process. Simply call any of the examples
with no arguments to see the help output. If your system has its IOMMU
enabled you can run the examples as your regular user. If it doesn't, you'll
need to run as a privileged user (root).

A good example to start with is `examples/nvme/identify`, which prints
out information about all of the NVMe devices on your system.

Larger, more fully functional applications are available in the `app`
directory. This includes the iSCSI and NVMe-oF target.
