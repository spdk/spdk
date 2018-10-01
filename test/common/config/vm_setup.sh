#!/usr/bin/env bash

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

set -e

VM_SETUP_PATH=$(readlink -f ${BASH_SOURCE%/*})

UPGRADE=false
INSTALL=false
CONF="librxe,iscsi,rocksdb,fio,flamegraph,tsocks,qemu,vpp,libiscsi,nvmecli"

function install_rxe_cfg()
{
    if echo $CONF | grep -q librxe; then
        # rxe_cfg is used in the NVMe-oF tests
        # The librxe-dev repository provides a command line tool called rxe_cfg which makes it
        # very easy to use Soft-RoCE. The build pool utilizes this command line tool in the absence
        # of any real RDMA NICs to simulate one for the NVMe-oF tests.
        if hash rxe_cfg 2> /dev/null; then
            echo "rxe_cfg is already installed. skipping"
        else
            if [ -d librxe-dev ]; then
                echo "librxe-dev source already present, not cloning"
            else
                git clone "${GIT_REPO_LIBRXE}"
            fi

            ./librxe-dev/configure --libdir=/usr/lib64/ --prefix=
            make -C librxe-dev -j${jobs}
            sudo make -C librxe-dev install
        fi
    fi
}

function install_iscsi_adm()
{
    if echo $CONF | grep -q iscsi; then
        # iscsiadm is used in the iscsi_tgt tests
        # The version of iscsiadm that ships with fedora 26 was broken as of November 3 2017.
        # There is already a bug report out about it, and hopefully it is fixed soon, but in the event that
        # that version is still broken when you do your setup, the below steps will fix the issue.
        CURRENT_VERSION=$(iscsiadm --version)
        OPEN_ISCSI_VER='iscsiadm version 6.2.0.874'
        if [ "$CURRENT_VERSION" == "$OPEN_ISCSI_VER" ]; then
            if [ ! -d open-iscsi-install ]; then
                mkdir -p open-iscsi-install/patches
                sudo dnf download --downloaddir=./open-iscsi-install --source iscsi-initiator-utils
                rpm2cpio open-iscsi-install/$(ls ~/open-iscsi-install) | cpio -D open-iscsi-install -idmv
                mv open-iscsi-install/00* open-iscsi-install/patches/
                git clone "${GIT_REPO_OPEN_ISCSI}" open-iscsi-install/open-iscsi

                # the configurations of username and email are needed for applying patches to iscsiadm.
                git -C open-iscsi-install/open-iscsi config user.name none
                git -C open-iscsi-install/open-iscsi config user.email none

                git -C open-iscsi-install/open-iscsi checkout 86e8892
                for patch in `ls open-iscsi-install/patches`; do
                    git -C open-iscsi-install/open-iscsi am ../patches/$patch
                done
                sed -i '427s/.*/-1);/' open-iscsi-install/open-iscsi/usr/session_info.c
                make -C open-iscsi-install/open-iscsi -j${jobs}
                sudo make -C open-iscsi-install/open-iscsi install
            else
                echo "custom open-iscsi install located, not reinstalling"
            fi
        fi
    fi
}

function install_rocksdb()
{
    if echo $CONF | grep -q rocksdb; then
        # Rocksdb is installed for use with the blobfs tests.
        if [ ! -d /usr/src/rocksdb ]; then
            git clone "${GIT_REPO_ROCKSDB}"
            git -C ./rocksdb checkout spdk-v5.6.1
            sudo mv rocksdb /usr/src/
        else
            sudo git -C /usr/src/rocksdb checkout spdk-v5.6.1
            echo "rocksdb already in /usr/src. Not checking out again"
        fi
    fi
}

function install_fio()
{
    if echo $CONF | grep -q fio; then
        # This version of fio is installed in /usr/src/fio to enable
        # building the spdk fio plugin.
        if [ ! -d /usr/src/fio ]; then
            if [ ! -d fio ]; then
                git clone "${GIT_REPO_FIO}"
                sudo mv fio /usr/src/
            else
                sudo mv fio /usr/src/
            fi
            (
                git -C /usr/src/fio checkout master &&
                git -C /usr/src/fio pull &&
                git -C /usr/src/fio checkout fio-3.3 &&
                make -C /usr/src/fio -j${jobs} &&
                sudo make -C /usr/src/fio install
            )
        else
            echo "fio already in /usr/src/fio. Not installing"
        fi
    fi
}

function install_flamegraph()
{
    if echo $CONF | grep -q flamegraph; then
        # Flamegraph is used when printing out timing graphs for the tests.
        if [ ! -d /usr/local/FlameGraph ]; then
            git clone "${GIT_REPO_FLAMEGRAPH}"
            mkdir -p /usr/local
            sudo mv FlameGraph /usr/local/FlameGraph
        else
            echo "flamegraph already installed. Skipping"
        fi
    fi
}

function install_qemu()
{
    if echo $CONF | grep -q qemu; then
        # Qemu is used in the vhost tests.
        SPDK_QEMU_BRANCH=spdk-2.12
        mkdir -p qemu
        if [ ! -d "qemu/$SPDK_QEMU_BRANCH" ]; then
            git -C ./qemu clone "${GIT_REPO_QEMU}" -b "$SPDK_QEMU_BRANCH" "$SPDK_QEMU_BRANCH"
        else
            echo "qemu already checked out. Skipping"
        fi

        declare -a opt_params=("--prefix=/usr/local/qemu/$SPDK_QEMU_BRANCH")

        # Most tsocks proxies rely on a configuration file in /etc/tsocks.conf.
        # If using tsocks, please make sure to complete this config before trying to build qemu.
        if echo $CONF | grep -q tsocks; then
            if hash tsocks 2> /dev/null; then
                opt_params+=(--with-git='tsocks git')
            fi
        fi

        # The qemu configure script places several output files in the CWD.
        (cd qemu/$SPDK_QEMU_BRANCH && ./configure "${opt_params[@]}" --target-list="x86_64-softmmu" --enable-kvm --enable-linux-aio --enable-numa)

        make -C ./qemu/$SPDK_QEMU_BRANCH -j${jobs}
        sudo make -C ./qemu/$SPDK_QEMU_BRANCH install
    fi
}

function install_vpp()
{
    if echo $CONF | grep -q vpp; then
        # Vector packet processing (VPP) is installed for use with iSCSI tests.
        # At least on fedora 28, the yum setup that vpp uses is deprecated and fails.
        # The actions taken under the vpp_setup script are necessary to fix this issue.
        if [ -d vpp_setup ]; then
            echo "vpp setup already done."
        else
            echo "%_topdir  $HOME/vpp_setup/src/rpm" >> ~/.rpmmacros
            sudo dnf install -y perl-generators
            mkdir -p ~/vpp_setup/src/rpm
            mkdir -p vpp_setup/src/rpm/BUILD vpp_setup/src/rpm/RPMS vpp_setup/src/rpm/SOURCES \
            vpp_setup/src/rpm/SPECS vpp_setup/src/rpm/SRPMS
            dnf download --downloaddir=./vpp_setup/src/rpm --source redhat-rpm-config
            rpm -ivh ~/vpp_setup/src/rpm/redhat-rpm-config*
            sed -i s/"Requires: (annobin if gcc)"//g ~/vpp_setup/src/rpm/SPECS/redhat-rpm-config.spec
            rpmbuild -ba ~/vpp_setup/src/rpm/SPECS/*.spec
            sudo dnf remove -y --noautoremove redhat-rpm-config
            sudo rpm -Uvh ~/vpp_setup/src/rpm/RPMS/noarch/*
        fi

        if [ -d vpp ]; then
            echo "vpp already cloned."
            if [ ! -d vpp/build-root ]; then
                echo "build-root has not been done"
                echo "remove the `pwd` and start again"
                exit 1
            fi
        else
            git clone "${GIT_REPO_VPP}"
            git -C ./vpp checkout v18.01.1
            # VPP 18.01.1 does not support OpenSSL 1.1.
            # For compilation, a compatibility package is used temporarily.
            sudo dnf install -y --allowerasing compat-openssl10-devel
            # Installing required dependencies for building VPP
            yes | make -C ./vpp install-dep

            make -C ./vpp pkg-rpm -j${jobs}
            # Reinstall latest OpenSSL devel package.
            sudo dnf install -y --allowerasing openssl-devel
            sudo dnf install -y \
                ./vpp/build_root/vpp-lib-18.01.1-release.x86_64.rpm \
                ./vpp/build_root/vpp-devel-18.01.1-release.x86_64.rpm \
                ./vpp/build_root/vpp-18.01.1-release.x86_64.rpm
            # Since hugepage configuration is done via spdk/scripts/setup.sh,
            # this default config is not needed.
            #
            # NOTE: Parameters kernel.shmmax and vm.max_map_count are set to
            # very low count and cause issues with hugepage total sizes above 1GB.
            sudo rm -f /etc/sysctl.d/80-vpp.conf
        fi
    fi
}

function install_nvmecli()
{
    if echo $CONF | grep -q nvmecli; then
        SPDK_NVME_CLI_BRANCH=spdk-1.6
        if [ ! -d nvme-cli ]; then
            git clone "${GIT_REPO_SPDK_NVME_CLI}" -b "$SPDK_NVME_CLI_BRANCH"
        else
            echo "nvme-cli already checked out. Skipping"
        fi
    fi
}

function install_libiscsi()
{
    if echo $CONF | grep -q libiscsi; then
        # We currently don't make any changes to the libiscsi repository for our tests, but it is possible that we will need
        # to later. Cloning from git is just future proofing the machines.
        if [ ! -d libiscsi ]; then
            git clone "${GIT_REPO_LIBISCSI}"
        else
            echo "libiscsi already checked out. Skipping"
        fi
        ( cd libiscsi && ./autogen.sh &&  ./configure --prefix=/usr/local/libiscsi)
        make -C ./libiscsi -j${jobs}
        sudo make -C ./libiscsi install
    fi
}

function usage()
{
    echo "This script is intended to automate the environment setup for a fedora linux virtual machine."
    echo "Please run this script as your regular user. The script will make calls to sudo as needed."
    echo ""
    echo "./vm_setup.sh"
    echo "  -h --help"
    echo "  -u --upgrade Run dnf upgrade"
    echo "  -i --install-deps Install dnf based dependencies"
    echo "  -t --test-conf List of test configurations to enable (${CONF})"
    echo "  -c --conf-path Path to configuration file"
    exit 0
}

while getopts 'iuht:c:-:' optchar; do
    case "$optchar" in
        -)
        case "$OPTARG" in
            help) usage;;
            upgrade) UPGRADE=true;;
            install-deps) INSTALL=true;;
            test-conf=*) CONF="${OPTARG#*=}";;
            conf-path=*) CONF_PATH="${OPTARG#*=}";;
            *) echo "Invalid argument '$OPTARG'"
            usage;;
        esac
        ;;
    h) usage;;
    u) UPGRADE=true;;
    i) INSTALL=true;;
    t) CONF="$OPTARG";;
    c) CONF_PATH="$OPTARG";;
    *) echo "Invalid argument '$OPTARG'"
    usage;;
    esac
done

if [ ! -z "$CONF_PATH" ]; then
    if [ ! -f "$CONF_PATH" ]; then
        echo Configuration file does not exist: "$CONF_PATH"
        exit 1
    else
        source "$CONF_PATH"
    fi
fi

cd ~

: ${GIT_REPO_SPDK=https://review.gerrithub.io/spdk/spdk}; export GIT_REPO_SPDK
: ${GIT_REPO_DPDK=https://github.com/spdk/dpdk.git}; export GIT_REPO_DPDK
: ${GIT_REPO_LIBRXE=https://github.com/SoftRoCE/librxe-dev.git}; export GIT_REPO_LIBRXE
: ${GIT_REPO_OPEN_ISCSI=https://github.com/open-iscsi/open-iscsi}; export GIT_REPO_OPEN_ISCSI
: ${GIT_REPO_ROCKSDB=https://review.gerrithub.io/spdk/rocksdb}; export GIT_REPO_ROCKSDB
: ${GIT_REPO_FIO=http://git.kernel.dk/fio.git}; export GIT_REPO_FIO
: ${GIT_REPO_FLAMEGRAPH=https://github.com/brendangregg/FlameGraph.git}; export GIT_REPO_FLAMEGRAPH
: ${GIT_REPO_QEMU=https://github.com/spdk/qemu}; export GIT_REPO_QEMU
: ${GIT_REPO_VPP=https://gerrit.fd.io/r/vpp}; export GIT_REPO_VPP
: ${GIT_REPO_LIBISCSI=https://github.com/sahlberg/libiscsi}; export GIT_REPO_LIBISCSI
: ${GIT_REPO_SPDK_NVME_CLI=https://github.com/spdk/nvme-cli}; export GIT_REPO_SPDK_NVME_CLI

jobs=$(($(nproc)*2))

if $UPGRADE; then
    sudo dnf upgrade -y
fi

if $INSTALL; then
    sudo dnf install -y git
fi

mkdir -p spdk_repo/output

if [ -d spdk_repo/spdk ]; then
    echo "spdk source already present, not cloning"
else
    git -C spdk_repo clone "${GIT_REPO_SPDK}"
fi
git -C spdk_repo/spdk config submodule.dpdk.url "${GIT_REPO_DPDK}"
git -C spdk_repo/spdk submodule update --init --recursive

if $INSTALL; then
    sudo ./scripts/pkgdep.sh

    if echo $CONF | grep -q tsocks; then
        sudo dnf install -y tsocks
    fi

    sudo dnf install -y \
    valgrind \
    jq \
    nvme-cli \
    ceph \
    gdb \
    fio \
    librbd-devel \
    kernel-devel \
    gflags-devel \
    libasan \
    libubsan \
    autoconf \
    automake \
    libtool \
    libmount-devel \
    iscsi-initiator-utils \
    isns-utils-devel \
    pmempool \
    perl-open \
    glib2-devel \
    pixman-devel \
    astyle-devel \
    elfutils \
    elfutils-libelf-devel \
    flex \
    bison \
    targetcli \
    perl-Switch \
    librdmacm-utils \
    libibverbs-utils \
    gdisk \
    socat \
    sshfs
fi

sudo mkdir -p /usr/src

install_rxe_cfg&
install_iscsi_adm&
install_rocksdb&
install_fio&
install_flamegraph&
install_qemu&
install_vpp&
install_nvmecli&
install_libiscsi&

wait
# create autorun-spdk.conf in home folder. This is sourced by the autotest_common.sh file.
# By setting any one of the values below to 0, you can skip that specific test. If you are
# using your autotest platform to do sanity checks before uploading to the build pool, it is
# probably best to only run the tests that you believe your changes have modified along with
# Scanbuild and check format. This is because running the whole suite of tests in series can
# take ~40 minutes to complete.
if [ ! -e ~/autorun-spdk.conf ]; then
	cat > ~/autorun-spdk.conf << EOF
# assign a value of 1 to all of the pertinent tests
SPDK_BUILD_DOC=1
SPDK_RUN_CHECK_FORMAT=1
SPDK_RUN_SCANBUILD=1
SPDK_RUN_VALGRIND=1
SPDK_TEST_UNITTEST=1
SPDK_TEST_ISCSI=1
SPDK_TEST_ISCSI_INITIATOR=1
# nvme and nvme-cli cannot be run at the same time on a VM.
SPDK_TEST_NVME=1
SPDK_TEST_NVME_CLI=0
SPDK_TEST_NVMF=1
SPDK_TEST_RBD=1
# requires some extra configuration. see TEST_ENV_SETUP_README
SPDK_TEST_VHOST=0
SPDK_TEST_VHOST_INIT=0
SPDK_TEST_BLOCKDEV=1
# doesn't work on vm
SPDK_TEST_IOAT=0
SPDK_TEST_EVENT=1
SPDK_TEST_BLOBFS=1
SPDK_TEST_PMDK=1
SPDK_TEST_LVOL=1
SPDK_RUN_ASAN=1
SPDK_RUN_UBSAN=1
EOF
fi
