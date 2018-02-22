#! /usr/bin/env bash

# Virtual Machine environment requirements:
# 8 GiB of RAM (for DPDK)
# enable intel_kvm on your host machine

# The purpose of this script is to provide a simple procedure for spinning up a new
# virtual test environment capable of running our whole test suite. This script, when
# applied to a fresh install of fedora 26 server will install all of the necessary dependencies
# to run almost the complete test suite. The main exception being VHost. Vhost requires the
# configuration of a second virtual machine. instructions for how to configure
# that vm are included in the file TEST_ENV_SETUP_README inside this repository

# it is important to enable nesting for vms in kernel command line of your machine for the vhost tests.
#     in /etc/default/grub
#     append the following to the GRUB_CMDLINE_LINUX line
#     intel_iommu=on kvm-intel.nested=1

# We have made a lot of progress with removing hardcoded paths from the tests,
# but it may be easiest if you create your user with the name sys_sgsw for now.

set -e

jobs=$(($(nproc)*2))

sudo dnf upgrade -y
sudo dnf install -y gcc
sudo dnf install -y gcc-c++
sudo dnf install -y make
sudo dnf install -y git
sudo dnf install -y jq
sudo dnf install -y valgrind
sudo dnf install -y nvme-cli
sudo dnf install -y ceph
sudo dnf install -y gdb
sudo dnf install -y sg3_utils
sudo dnf install -y fio
sudo dnf install -y librbd-devel
sudo dnf install -y kernel-devel
sudo dnf install -y gflags-devel
sudo dnf install -y libasan
sudo dnf install -y libubsan
sudo dnf install -y autoconf
sudo dnf install -y automake
sudo dnf install -y libtool
sudo dnf install -y libmount-devel
sudo dnf install -y isns-utils-devel
sudo dnf install -y openssl-devel
sudo dnf install -y numactl-devel
sudo dnf install -y libaio-devel
sudo dnf install -y CUnit-devel
sudo dnf install -y clang-analyzer
sudo dnf install -y libpmemblk-devel
sudo dnf install -y libibverbs libibverbs-devel librdmacm librdmacm-devel
sudo dnf install -y perl-open
sudo dnf install -y glib2-devel
sudo dnf install -y pixman-devel
sudo dnf install -y libiscsi-devel
sudo dnf install -y doxygen
sudo dnf install -y astyle-devel
sudo dnf install -y python
sudo dnf install -y python-pep8
sudo dnf install -y lcov
sudo dnf install -y libuuid-devel
sudo dnf install -y elfutils-libelf-devel
sudo dnf install -y flex
sudo dnf install -y bison
sudo dnf install -y targetcli

cd ~

mkdir -p spdk_repo

# the configurations of username and email are needed later for applying patches to iscsiadm.
git config --global user.name none
git config --global user.email none

# The librxe-dev repository provides a command line tool called rxe_cfg which makes it
# very easy to use Soft-RoCE. The build pool utilizes this command line tool in the absence
# of any real RDMA NICs to simulate one for the NVMe-oF tests.
git clone https://github.com/SoftRoCE/librxe-dev.git
sudo dnf install -y perl-Switch librdmacm-utils libibverbs-utils

cd librxe-dev
./configure --libdir=/usr/lib64/ --prefix=
make -j${jobs}
sudo make install
cd ~

cd spdk_repo
mkdir -p output
git clone https://review.gerrithub.io/spdk/spdk
cd spdk
git submodule update --init --recursive
cd ~

# The version of iscsiadm that ships with fedora 26 was broken as of November 3 2017.
# There is already a bug report out about it, and hopefully it is fixed soon, but in the event that
# that version is still broken when you do your setup, the below steps will fix the issue.
CURRENT_VERSION=$(iscsiadm --version)
OPEN_ISCSI_VER='iscsiadm version 6.2.0.874'
if [ "$CURRENT_VERSION" == "$OPEN_ISCSI_VER" ]; then
    mkdir -p open-iscsi-install
    cd open-iscsi-install
    sudo dnf download --source iscsi-initiator-utils
    rpm2cpio iscsi-initiator-utils-6.2.0.874-3.git86e8892.fc26.src.rpm | cpio -idmv
    mkdir -p patches
    mv 00* patches/
    git clone https://github.com/open-iscsi/open-iscsi

    cd open-iscsi
    git checkout 86e8892
    for patch in `ls ../patches`; do
        git am ../patches/$patch
    done
    sed -i '427s/.*/-1);/' usr/session_info.c
    make -j${jobs}
    sudo make install
    cd ~
fi


sudo mkdir -p /usr/src

# Rocksdb is installed for use with the blobfs tests.
git clone https://review.gerrithub.io/spdk/rocksdb
git -C ./rocksdb checkout spdk-v5.6.1
sudo mv rocksdb /usr/src/

git clone http://git.kernel.dk/fio.git
sudo mv fio /usr/src/
(
    cd /usr/src/fio &&
    git checkout fio-3.3 &&
    make -j${jobs} &&
    sudo make install
)
cd ~

git clone https://github.com/brendangregg/FlameGraph.git
mkdir -p /usr/local
sudo mv FlameGraph /usr/local/FlameGraph

SPDK_QEMU_BRANCH=spdk-2.12-pre
mkdir -p qemu
cd qemu
git clone https://github.com/spdk/qemu -b "$SPDK_QEMU_BRANCH" "$SPDK_QEMU_BRANCH"
cd "$SPDK_QEMU_BRANCH"
if hash tsocks &> /dev/null; then
    git_param="--with-git='tsocks git'"
fi
./configure "$git_param" --prefix=/usr/local/qemu/$SPDK_QEMU_BRANCH --target-list="x86_64-softmmu" --enable-kvm --enable-linux-aio --enable-numa
make -j${jobs}
sudo make install
cd ~

# We currently don't make any changes to the libiscsi repository for our tests, but it is possible that we will need
# to later. Cloning from git is just future proofing the machines.
git clone https://github.com/sahlberg/libiscsi
cd libiscsi
./autogen.sh
./configure --prefix=/usr/local/libiscsi
make -j${jobs}
sudo make install


# create autorun-spdk.conf in home folder. This is sourced by the autotest_common.sh file.
# By setting any one of the values below to 0, you can skip that specific test. If you are
# using your autotest platform to do sanity checks before uploading to the build pool, it is
# probably best to only run the tests that you believe your changes have modified along with
# Scanbuild and check format. This is because running the whole suite of tests in series can
# take ~40 minutes to complete.
cat > ~/autorun-spdk.conf << EOF
# assign a value of 1 to all of the pertinent tests
SPDK_BUILD_DOC=1
SPDK_BUILD_IOAT_KMOD=1
SPDK_RUN_CHECK_FORMAT=1
SPDK_RUN_SCANBUILD=1
SPDK_RUN_VALGRIND=1
SPDK_TEST_UNITTEST=1
SPDK_TEST_ISCSI=1
SPDK_TEST_NVME=1
SPDK_TEST_NVMF=1
SPDK_TEST_RBD=1
# requires some extra configuration. see TEST_ENV_SETUP_README
SPDK_TEST_VHOST=0
SPDK_TEST_BLOCKDEV=1
# doesn't work on vm
SPDK_TEST_IOAT=0
SPDK_TEST_EVENT=1
SPDK_TEST_BLOBFS=1
SPDK_TEST_NVML=1
SPDK_RUN_ASAN=1
SPDK_RUN_UBSAN=1
EOF
