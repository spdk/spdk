#!/usr/bin/env bash

# Virtual Machine environment requirements:
# 8 GiB of RAM (for DPDK)
# enable intel_kvm on your host machine

# The purpose of this script is to provide a simple procedure for spinning up a new
# virtual test environment capable of running our whole test suite. This script, when
# applied to a fresh install of fedora 26 or ubuntu 16,18 server will install all of the
# necessary dependencies to run almost the complete test suite. The main exception being VHost.
# Vhost requires the configuration of a second virtual machine. instructions for how to configure
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
CONF="librxe,iscsi,rocksdb,fio,flamegraph,tsocks,qemu,vpp,libiscsi,nvmecli,qat"
LIBRXE_INSTALL=true

if [ $(uname -s) == "FreeBSD" ]; then
    OSID="freebsd"
    OSVERSION=$(freebsd-version | cut -d. -f1)
    PACKAGEMNG='pkg'
else
    OSID=$(source /etc/os-release && echo $ID)
    OSVERSION=$(source /etc/os-release && echo $VERSION_ID)
    PACKAGEMNG='undefined'
fi

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
                for patch in open-iscsi-install/patches/*; do
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

function install_qat()
{

    if [ "$PACKAGEMNG" = "dnf" ]; then
        sudo dnf install -y libudev-devel
    elif [ "$PACKAGEMNG" = "apt-get" ]; then
        sudo apt-get install -y libudev-dev
    fi

    if echo $CONF | grep -q qat; then
        qat_tarball=$(basename $DRIVER_LOCATION_QAT)
        kernel_maj=$(uname -r | cut -d'.' -f1)
        kernel_min=$(uname -r | cut -d'.' -f2)

        sudo modprobe -r qat_c62x
        if [ -d /QAT ]; then
            sudo rm -rf /QAT/
        fi

        sudo mkdir /QAT

        wget $DRIVER_LOCATION_QAT
        sudo cp $qat_tarball /QAT/
        (cd /QAT && sudo tar zxof /QAT/$qat_tarball)

        #The driver version 1.7.l.4.3.0-00033 contains a reference to a deprecated function. Remove it so the build won't fail.
        if [ $kernel_maj -le 4 ]; then
            if [ $kernel_min -le 17 ]; then
                sudo sed -i 's/rdtscll(timestamp);/timestamp = rdtsc_ordered();/g' \
                /QAT/quickassist/utilities/osal/src/linux/kernel_space/OsalServices.c || true
            fi
        fi

        (cd /QAT && sudo ./configure --enable-icp-sriov=host && sudo make install)

        if sudo service qat_service start; then
            echo "failed to start the qat service. Something may be wrong with your device or package."
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
        local fio_version="fio-3.15"

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
                git -C /usr/src/fio checkout $fio_version &&
                if [ $OSID == 'freebsd' ]; then
                    gmake -C /usr/src/fio -j${jobs} &&
                    sudo gmake -C /usr/src/fio install
                else
                    make -C /usr/src/fio -j${jobs} &&
                    sudo make -C /usr/src/fio install
                fi
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
        # Two versions of QEMU are used in the tests.
        # Stock QEMU is used for vhost. A special fork
        # is used to test OCSSDs. Install both.

        # Packaged QEMU
        if [ "$PACKAGEMNG" = "dnf" ]; then
                sudo dnf install -y qemu-system-x86 qemu-img
        elif [ "$PACKAGEMNG" = "apt-get" ]; then
                sudo apt-get install -y qemu-system-x86 qemu-img
        elif [ "$PACKAGEMNG" = "pacman" ]; then
                sudo pacman -Sy --needed --noconfirm qemu
        fi

        # Forked QEMU
        SPDK_QEMU_BRANCH=spdk-3.0.0
        mkdir -p qemu
        if [ ! -d "qemu/$SPDK_QEMU_BRANCH" ]; then
            git -C ./qemu clone "${GIT_REPO_QEMU}" -b "$SPDK_QEMU_BRANCH" "$SPDK_QEMU_BRANCH"
        else
            echo "qemu already checked out. Skipping"
        fi

        declare -a opt_params=("--prefix=/usr/local/qemu/$SPDK_QEMU_BRANCH")
        if [ "$PACKAGEMNG" = "pacman" ]; then
            # GCC 9 on ArchLinux fails to compile Qemu due to some old warnings which were not detected by older versions.
            opt_params+=("--extra-cflags=-Wno-error=stringop-truncation -Wno-error=deprecated-declarations -Wno-error=incompatible-pointer-types -Wno-error=format-truncation")
            opt_params+=("--disable-glusterfs")
        fi

        # Most tsocks proxies rely on a configuration file in /etc/tsocks.conf.
        # If using tsocks, please make sure to complete this config before trying to build qemu.
        if echo $CONF | grep -q tsocks; then
            if hash tsocks 2> /dev/null; then
                opt_params+=("--with-git='tsocks git'")
            fi
        fi

        sed -i s@git://git.qemu.org/@https://github.com/qemu/@g qemu/$SPDK_QEMU_BRANCH/.gitmodules
        sed -i s@git://git.qemu.org/@https://github.com/qemu/@g qemu/$SPDK_QEMU_BRANCH/.git/config
        sed -i s@git://git.qemu-project.org/@https://github.com/qemu/@g qemu/$SPDK_QEMU_BRANCH/.gitmodules
        sed -i s@git://git.qemu-project.org/@https://github.com/qemu/@g qemu/$SPDK_QEMU_BRANCH/.git/config
        # The qemu configure script places several output files in the CWD.
        (cd qemu/$SPDK_QEMU_BRANCH && ./configure "${opt_params[@]}" --target-list="x86_64-softmmu" --enable-kvm --enable-linux-aio --enable-numa)

        make -C ./qemu/$SPDK_QEMU_BRANCH -j${jobs}
        sudo make -C ./qemu/$SPDK_QEMU_BRANCH install
    fi
}

function install_vpp()
{
    if echo $CONF | grep -q vpp; then
        if [ -d /usr/local/src/vpp ]; then
            echo "vpp already cloned."
            if [ ! -d /usr/local/src/vpp/build-root ]; then
                echo "build-root has not been done"
                echo "remove the $(pwd) and start again"
                exit 1
            fi
        else
            git clone "${GIT_REPO_VPP}"
            git -C ./vpp checkout v19.04.2

            if [ "${OSID}" == 'fedora' ]; then
                if [ ${OSVERSION} -eq 29 ]; then
                    git -C ./vpp apply ${VM_SETUP_PATH}/patch/vpp/fedora29-fix.patch
                fi
                if [ ${OSVERSION} -eq 30 ]; then
                    git -C ./vpp apply ${VM_SETUP_PATH}/patch/vpp/fedora30-fix.patch
                fi
            fi

            # Installing required dependencies for building VPP
            yes | make -C ./vpp install-dep

            make -C ./vpp build -j${jobs}

            sudo mv ./vpp /usr/local/src/vpp-19.04
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
	if [ ! -d "/usr/local/src/nvme-cli" ]; then
            git clone "https://review.spdk.io/spdk/nvme-cli" "nvme-cli-cuse"
            git -C ./nvme-cli-cuse checkout nvme-cuse
            make -C ./nvme-cli-cuse
            sudo mv ./nvme-cli-cuse /usr/local/src/nvme-cli
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

function install_git() {
    sudo yum install -y zlib-devel curl-devel
    tar -xzof <(wget -qO- "$GIT_REPO_GIT")
    (cd git-${GIT_VERSION} && ./configure --prefix=/usr/local/git && sudo  make -j${jobs} install)
    sudo sh -c "echo 'export PATH=/usr/local/git/bin:$PATH' >> /etc/bashrc"
    source /etc/bashrc
}

function usage()
{
    echo "This script is intended to automate the environment setup for a linux virtual machine."
    echo "Please run this script as your regular user. The script will make calls to sudo as needed."
    echo ""
    echo "./vm_setup.sh"
    echo "  -h --help"
    echo "  -u --upgrade Run $PACKAGEMNG upgrade"
    echo "  -i --install-deps Install $PACKAGEMNG based dependencies"
    echo "  -t --test-conf List of test configurations to enable (${CONF})"
    echo "  -c --conf-path Path to configuration file"
    exit 0
}

# Get package manager #
if hash yum &>/dev/null; then
    PACKAGEMNG=yum
elif hash dnf &>/dev/null; then
    PACKAGEMNG=dnf
elif hash apt-get &>/dev/null; then
    PACKAGEMNG=apt-get
elif hash pacman &>/dev/null; then
    PACKAGEMNG=pacman
elif hash pkg &>/dev/null; then
    PACKAGEMNG=pkg
else
    echo 'Supported package manager not found. Script supports "dnf" and "apt-get".'
fi

if [ $PACKAGEMNG == 'apt-get' ] && [ $OSID != 'ubuntu' ]; then
    echo 'Located apt-get package manager, but it was tested for Ubuntu only'
fi
if [ $PACKAGEMNG == 'dnf' ] && [ $OSID != 'fedora' ]; then
    echo 'Located dnf package manager, but it was tested for Fedora only'
fi

# Parse input arguments #
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

if [ -n "$CONF_PATH" ]; then
    if [ ! -f "$CONF_PATH" ]; then
        echo Configuration file does not exist: "$CONF_PATH"
        exit 1
    else
        source "$CONF_PATH"
    fi
fi

cd ~
GIT_VERSION=2.25.1
: ${GIT_REPO_SPDK=https://github.com/spdk/spdk.git}; export GIT_REPO_SPDK
: ${GIT_REPO_DPDK=https://github.com/spdk/dpdk.git}; export GIT_REPO_DPDK
: ${GIT_REPO_LIBRXE=https://github.com/SoftRoCE/librxe-dev.git}; export GIT_REPO_LIBRXE
: ${GIT_REPO_OPEN_ISCSI=https://github.com/open-iscsi/open-iscsi}; export GIT_REPO_OPEN_ISCSI
: ${GIT_REPO_ROCKSDB=https://review.spdk.io/spdk/rocksdb}; export GIT_REPO_ROCKSDB
: ${GIT_REPO_FIO=http://git.kernel.dk/fio.git}; export GIT_REPO_FIO
: ${GIT_REPO_FLAMEGRAPH=https://github.com/brendangregg/FlameGraph.git}; export GIT_REPO_FLAMEGRAPH
: ${GIT_REPO_QEMU=https://github.com/spdk/qemu}; export GIT_REPO_QEMU
: ${GIT_REPO_VPP=https://gerrit.fd.io/r/vpp}; export GIT_REPO_VPP
: ${GIT_REPO_LIBISCSI=https://github.com/sahlberg/libiscsi}; export GIT_REPO_LIBISCSI
: ${GIT_REPO_SPDK_NVME_CLI=https://github.com/spdk/nvme-cli}; export GIT_REPO_SPDK_NVME_CLI
: ${GIT_REPO_INTEL_IPSEC_MB=https://github.com/spdk/intel-ipsec-mb.git}; export GIT_REPO_INTEL_IPSEC_MB
: ${DRIVER_LOCATION_QAT=https://01.org/sites/default/files/downloads//qat1.7.l.4.9.0-00008.tar.gz}; export DRIVER_LOCATION_QAT
: ${GIT_REPO_GIT=https://github.com/git/git/archive/v${GIT_VERSION}.tar.gz}; export GIT_REPO_GIT

if [ $PACKAGEMNG == 'pkg' ]; then
    jobs=$(( $(sysctl -n hw.ncpu) * 2 ))
else
    jobs=$(($(nproc)*2))
fi

if $UPGRADE; then
    if [ $PACKAGEMNG == 'yum' ]; then
        sudo $PACKAGEMNG upgrade -y
    elif [ $PACKAGEMNG == 'dnf' ]; then
        sudo $PACKAGEMNG upgrade -y
    elif [ $PACKAGEMNG == 'apt-get' ]; then
        sudo $PACKAGEMNG update
        sudo $PACKAGEMNG upgrade -y
    elif [ $PACKAGEMNG == 'pacman' ]; then
        sudo $PACKAGEMNG -Syu --noconfirm --needed
    elif [ $PACKAGEMNG == 'pkg' ]; then
        sudo $PACKAGEMNG upgrade -y
    fi
fi

if $INSTALL; then
    if [ $PACKAGEMNG == 'pacman' ]; then
        sudo $PACKAGEMNG -Sy --needed --noconfirm git
    else
        if [ "${OSID} ${OSVERSION}" == 'centos 7' ]; then
            install_git
        else
            sudo $PACKAGEMNG install -y git
        fi
    fi
fi

mkdir -p spdk_repo/output || echo "Can not create spdk_repo/output directory."

if [ -d spdk_repo/spdk ]; then
    echo "spdk source already present, not cloning"
else
    git -C spdk_repo clone "${GIT_REPO_SPDK}"
fi
git -C spdk_repo/spdk config submodule.dpdk.url "${GIT_REPO_DPDK}"
git -C spdk_repo/spdk config submodule.intel-ipsec-mb.url "${GIT_REPO_INTEL_IPSEC_MB}"
git -C spdk_repo/spdk submodule update --init --recursive

if $INSTALL; then
    if [ "${OSID} ${OSVERSION}" == 'centos 8' ]; then
        #During install using vm_setup.sh there is error with AppStream, to fix it we need to refresh yum
        sudo yum update -y --refresh
    fi
    sudo spdk_repo/spdk/scripts/pkgdep.sh --all

    if [ $PACKAGEMNG == 'pkg' ]; then
        sudo pkg install -y pciutils \
        jq \
        gdb \
        fio \
        p5-extutils-pkgconfig \
        libtool \
        flex \
        bison \
        gdisk \
        socat \
        sshpass \
        py37-pandas

    elif [ $PACKAGEMNG == 'yum' ]; then
        sudo yum install -y pciutils \
        valgrind \
        jq \
        nvme-cli \
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
        isns-utils-devel\
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
        sshfs \
        sshpass \
        python3-pandas \
        rpm-build \
        iptables \
        clang-analyzer \
        bc \
        kernel-modules-extra \
        systemd-devel \
        python3

        sudo yum install -y nbd || {
            wget -O nbd.rpm https://download-ib01.fedoraproject.org/pub/epel/7/x86_64/Packages/n/nbd-3.14-2.el7.x86_64.rpm
            sudo yum install -y nbd.rpm
        }

    elif [ $PACKAGEMNG == 'dnf' ]; then
        if echo $CONF | grep -q tsocks; then
            # currently, tsocks package is retired in fedora 31, so don't exit in case
            # installation failed
            # FIXME: Review when fedora starts to successfully build this package again.
            sudo dnf install -y tsocks || echo "Installation of the tsocks package failed, proxy may not be available"
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
        libabigail \
        elfutils-libelf-devel \
        flex \
        bison \
        targetcli \
        perl-Switch \
        librdmacm-utils \
        libibverbs-utils \
        gdisk \
        socat \
        sshfs \
        sshpass \
        python3-pandas \
        btrfs-progs \
        rpm-build \
        iptables \
        clang-analyzer \
        bc \
        kernel-modules-extra \
        systemd-devel \
        smartmontools

    elif [ $PACKAGEMNG == 'apt-get' ]; then
        echo "Package perl-open is not available at Ubuntu repositories" >&2

        if echo $CONF | grep -q tsocks; then
            sudo apt-get install -y tsocks
        fi

        # asan an ubsan have to be installed together to not mix up gcc versions
        if sudo apt-get install -y libasan5; then
            sudo apt-get install -y libubsan1
        else
            echo "Latest libasan5 is not available" >&2
            echo "  installing libasan2 and corresponding libubsan0" >&2
            sudo apt-get install -y libasan2
            sudo apt-get install -y libubsan0
        fi
        if ! sudo apt-get install -y rdma-core; then
            echo "Package rdma-core is avaliable at Ubuntu 18 [universe] repositorium" >&2
            sudo apt-get install -y rdmacm-utils
            sudo apt-get install -y ibverbs-utils
        else
            LIBRXE_INSTALL=false
        fi
        if ! sudo apt-get install -y libpmempool1; then
            echo "Package libpmempool1 is available at Ubuntu 18 [universe] repositorium" >&2
        fi
        if ! sudo apt-get install -y clang-tools; then
            echo "Package clang-tools is available at Ubuntu 18 [universe] repositorium" >&2
        fi
        if ! sudo apt-get install -y --no-install-suggests --no-install-recommends open-isns-utils; then
            echo "Package open-isns-utils is available at Ubuntu 18 [universe] repositorium" >&2
        fi

        # Package name for Ubuntu 18 is targetcli-fb but for Ubuntu 16 it's targetcli
        if ! sudo apt-get install -y targetcli-fb; then
            sudo apt-get install -y targetcli
        fi

        sudo apt-get install -y \
        valgrind \
        jq \
        nvme-cli \
        ceph \
        gdb \
        fio \
        librbd-dev \
        linux-headers-generic \
        libgflags-dev \
        autoconf \
        automake \
        libtool \
        libmount-dev \
        open-iscsi \
        libglib2.0-dev \
        libpixman-1-dev \
        astyle \
        elfutils \
        libelf-dev \
        flex \
        bison \
        libswitch-perl \
        gdisk \
        socat \
        sshfs \
        sshpass \
        python3-pandas \
        btrfs-tools \
        bc \
        smartmontools

        # rpm-build is not used
        # iptables installed by default

    elif [ $PACKAGEMNG == 'pacman' ]; then
        if echo $CONF | grep -q tsocks; then
            sudo pacman -Sy --noconfirm --needed tsocks
        fi

        sudo pacman -Sy --noconfirm --needed valgrind \
            jq \
            nvme-cli \
            ceph \
            gdb \
            fio \
            linux-headers \
            gflags \
            autoconf \
            automake \
            libtool \
            libutil-linux \
            libiscsi \
            open-isns \
            glib2 \
            pixman \
            flex \
            bison \
            elfutils \
            libelf \
            astyle \
            gptfdisk \
            socat \
            sshfs \
            sshpass \
            python-pandas \
            btrfs-progs \
            iptables \
            clang \
            bc \
            perl-switch \
            open-iscsi \
            smartmontools \
            parted

        # TODO:
        # These are either missing or require some other installation method
        # than pacman:

        # librbd-devel
        # perl-open
        # targetcli

    else
        echo "Package manager is undefined, skipping INSTALL step"
    fi
fi

sudo mkdir -p /usr/src

if [ $OSID != 'freebsd' ]; then
    if [ $LIBRXE_INSTALL = true ]; then
        #Ubuntu18 integrates librxe to rdma-core, libibverbs-dev no longer ships infiniband/driver.h.
        #Don't compile librxe on ubuntu18 or later version, install package rdma-core instead.
        install_rxe_cfg&
    fi
    install_iscsi_adm&
    install_libiscsi&
    instll_vpp&
    install_nvmecli&
    install_qat&
    install_rocksdb&
    install_flamegraph&
    install_qemu&
fi
install_fio&

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
SPDK_RUN_VALGRIND=1
SPDK_TEST_CRYPTO=1
SPDK_RUN_FUNCTIONAL_TEST=1
SPDK_TEST_AUTOBUILD=1
SPDK_TEST_UNITTEST=1
SPDK_TEST_ISCSI=1
SPDK_TEST_ISCSI_INITIATOR=1
SPDK_TEST_NVME=1
SPDK_TEST_NVME_CLI=1
SPDK_TEST_NVMF=1
SPDK_TEST_RBD=1
SPDK_TEST_BLOCKDEV=1
SPDK_TEST_BLOBFS=1
SPDK_TEST_PMDK=1
SPDK_TEST_LVOL=1
SPDK_TEST_JSON=1
SPDK_RUN_ASAN=1
SPDK_RUN_UBSAN=1
# doesn't work on vm
SPDK_TEST_IOAT=0
# requires some extra configuration. see TEST_ENV_SETUP_README
SPDK_TEST_VHOST=0
SPDK_TEST_VHOST_INIT=0
# Not configured here
SPDK_RUN_INSTALLED_DPDK=0

EOF
fi
