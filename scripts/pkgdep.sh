#!/usr/bin/env bash
# Please run this script as root.

set -e

function usage()
{
	echo ""
	echo "This script is intended to automate the installation of package dependencies to build SPDK."
	echo "Please run this script as root user or with sudo -E."
	echo ""
	echo "$0"
	echo "  -h --help"
	echo "  -d --developer-tools        Install tools for developers (code styling, code coverage, etc.)"
	echo "  -f --fuse                   Additional dependencies for FUSE and CUSE"
	echo "                              For Ubuntu distribution we used external package sources."
	echo "  --rdma                      Additional dependencies for RDMA transport in NVMe over Fabrics"
	echo "  --docs                      Additional dependencies for building docs"
	echo ""
	exit 0
}

INSTALL_CRYPTO=false
INSTALL_DEV_TOOLS=false
INSTALL_FUSE=false
INSTALL_RDMA=false
INSTALL_DOCS=false

while getopts 'dhfi-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage;;
			developer-tools) INSTALL_DEV_TOOLS=true;;
			fuse) INSTALL_FUSE=true;;
			rdma) INSTALL_RDMA=true;;
			docs) INSTALL_DOCS=true;;
			*) echo "Invalid argument '$OPTARG'"
			usage;;
		esac
		;;
	h) usage;;
	d) INSTALL_DEV_TOOLS=true;;
	f) INSTALL_FUSE=true;;
	*) echo "Invalid argument '$OPTARG'"
	usage;;
	esac
done

trap 'set +e; trap - ERR; echo "Error!"; exit 1;' ERR

scriptsdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $scriptsdir/..)

if [ -s /etc/redhat-release ]; then
	. /etc/os-release
	# Minimal install
	yum install -y gcc gcc-c++ make CUnit-devel libaio-devel openssl-devel \
		libuuid-devel libiscsi-devel python
	# Additional dependencies for SPDK CLI - not available in rhel and centos
	if ! echo "$ID $VERSION_ID" | grep -E -q 'rhel 7|centos 7'; then
		yum install -y python3-configshell python3-pexpect
	fi
	# Additional dependencies for ISA-L used in compression
	yum install -y autoconf automake libtool help2man
	# Additional dependencies for building pmem based backends
	yum install -y libpmemblk-devel || true
	# Additional dependencies for DPDK
	yum install -y numactl-devel nasm
	if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
		# Tools for developers
		# Includes Fedora, CentOS 7, RHEL 7
		# Add EPEL repository for CUnit-devel and libunwind-devel
		if echo "$ID $VERSION_ID" | grep -E -q 'rhel 7|centos 7'; then
			if ! rpm --quiet -q epel-release; then
				yum install https://dl.fedoraproject.org/pub/epel/epel-release-latest-7.noarch.rpm
			fi

			if [[ $ID = 'rhel' ]]; then
				subscription-manager repos --enable "rhel-*-optional-rpms" --enable "rhel-*-extras-rpms"
			elif [[ $ID = 'centos' ]]; then
				yum --enablerepo=extras install -y epel-release
			fi
		fi
		yum install -y git astyle python-pycodestyle lcov \
			sg3_utils pciutils ShellCheck
		# Additional (optional) dependencies for showing backtrace in logs
		yum install -y libunwind-devel || true
	fi
	if [[ $INSTALL_FUSE == "true" ]]; then
		# Additional dependencies for FUSE and CUSE
		yum install -y fuse3-devel
	fi
	if [[ $INSTALL_RDMA == "true" ]]; then
		# Additional dependencies for RDMA transport in NVMe over Fabrics
		yum install -y libibverbs-devel librdmacm-devel
	fi
	if [[ $INSTALL_DOCS == "true" ]]; then
		# Additional dependencies for building docs
		yum install -y doxygen mscgen graphviz
	fi
elif [ -f /etc/debian_version ]; then
	. /etc/os-release
	VERSION_ID=$(sed 's/\.//g' <<< $VERSION_ID)
	# Includes Ubuntu, Debian
	# Minimal install
	apt-get install -y gcc g++ make libcunit1-dev libaio-dev libssl-dev \
		uuid-dev libiscsi-dev python
	# Additional dependencies for SPDK CLI - not available on older Ubuntus
	apt-get install -y python3-configshell-fb python3-pexpect || echo \
		"Note: Some SPDK CLI dependencies could not be installed."

	# Additional dependencies for DPDK
	if [[ $NAME == "Ubuntu" ]] && [[ $VERSION_ID -lt 1900 ]]; then
		# Adding repository with NASM version 2.13.03 for Ubuntu 18
		echo "This repository contains NASM version 2.13.03 for Ubuntu 18 needed for DPDK"
		add-apt-repository ppa:sergey-dryabzhinsky/ffmpeg
		apt-get update
	fi
	apt-get install -y libnuma-dev nasm
	# Additional dependencies for ISA-L used in compression
	apt-get install -y autoconf automake libtool help2man
	if [[ $NAME == "Ubuntu" ]] && [[ $VERSION_ID -gt 1800 ]]; then
		apt-get install -y libpmem-dev
	fi
	if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
		# Tools for developers
		apt-get install -y git astyle pep8 lcov clang sg3-utils pciutils shellcheck
		# Additional python style checker not available on ubuntu 16.04 or earlier.
		apt-get install -y pycodestyle || true
		# Additional (optional) dependencies for showing backtrace in logs
		apt-get install -y libunwind-dev || true
		# Additional dependecies for nvmf performance test script
		apt-get install -y python3-paramiko
	fi
	if [[ $INSTALL_FUSE == "true" ]]; then
		# Additional dependencies for FUSE and CUSE
		if [[ $NAME == "Ubuntu" ]] && [[ $VERSION_ID -gt 1400 ]] && [[ $VERSION_ID -lt 1900 ]]; then
			# Adding repository with libfuse3-dev for Ubuntu 14, 16 and 18
			echo "This repository contains libfuse3-dev for Ubuntu $VERSION needed for FUSE and CUSE"
			add-apt-repository ppa:bkryza/fuse3
			apt-get update
		fi
		apt-get install -y libfuse3-dev
	fi
	if [[ $INSTALL_RDMA == "true" ]]; then
		# Additional dependencies for RDMA transport in NVMe over Fabrics
		apt-get install -y libibverbs-dev librdmacm-dev
	fi
	if [[ $INSTALL_DOCS == "true" ]]; then
		# Additional dependencies for building docs
		apt-get install -y doxygen mscgen graphviz
	fi
elif [ -f /etc/SuSE-release ] || [ -f /etc/SUSE-brand ]; then
	# Minimal install
	zypper install -y gcc gcc-c++ make cunit-devel libaio-devel libopenssl-devel \
		libuuid-devel python-base
	# Additional dependencies for DPDK
	zypper install -y libnuma-devel nasm
	# Additional dependencies for ISA-L used in compression
	zypper install -y autoconf automake libtool help2man
	# Additional dependencies for building pmem based backends
	zypper install -y libpmemblk-devel
	if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
		# Tools for developers
		zypper install -y git-core lcov python-pycodestyle sg3_utils \
			pciutils ShellCheck
		# Additional (optional) dependencies for showing backtrace in logs
		zypper install libunwind-devel || true
	fi
	if [[ $INSTALL_FUSE == "true" ]]; then
		# Additional dependencies for FUSE and CUSE
		zypper install -y fuse3-devel
	fi
	if [[ $INSTALL_RDMA == "true" ]]; then
		# Additional dependencies for RDMA transport in NVMe over Fabrics
		zypper install -y rdma-core-devel
	fi
	if [[ $INSTALL_DOCS == "true" ]]; then
		# Additional dependencies for building docs
		zypper install -y doxygen mscgen graphviz
	fi
elif [ $(uname -s) = "FreeBSD" ] ; then
	# Minimal install
	pkg install -y gmake cunit openssl git bash misc/e2fsprogs-libuuid python
	# Additional dependencies for ISA-L used in compression
	pkg install -y autoconf automake libtool help2man
	if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
		# Tools for developers
		pkg install -y devel/astyle bash py27-pycodestyle \
			misc/e2fsprogs-libuuid sysutils/sg3_utils nasm
	fi
	if [[ $INSTALL_DOCS == "true" ]]; then
		# Additional dependencies for building docs
		pkg install -y doxygen mscgen graphviz
	fi
elif [ -f /etc/arch-release ]; then
	# Install main dependencies
	pacman -Sy --needed --noconfirm gcc make cunit libaio openssl \
		libutil-linux libiscsi python
	# Additional dependencies for SPDK CLI
	pacman -Sy --needed --noconfirm python-pexpect python-pip
	pip install configshell_fb
	# Additional dependencies for DPDK
	pacman -Sy --needed --noconfirm numactl nasm
	# Additional dependencies for ISA-L used in compression
	pacman -Sy --needed --noconfirm autoconf automake libtool help2man
	# Additional dependencies for building pmem based backends
	pacman -Sy --needed --noconfirm ndctl
	git clone https://github.com/pmem/pmdk.git /tmp/pmdk -b 1.6.1
	make -C /tmp/pmdk -j$(nproc)
	make install -C /tmp/pmdk
	echo "/usr/local/lib" > /etc/ld.so.conf.d/pmdk.conf
	ldconfig
	rm -rf /tmp/pmdk
	if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
		# Tools for developers
		pacman -Sy --needed --noconfirm git astyle autopep8 \
			clang sg3_utils pciutils shellcheck
		# Additional (optional) dependencies for showing backtrace in logs
		pacman -Sy --needed --noconfirm libunwind
		#fakeroot needed to instal via makepkg
		pacman -Sy --needed --noconfirm fakeroot
		su - $SUDO_USER -c "pushd /tmp;
			git clone https://aur.archlinux.org/perl-perlio-gzip.git;
			cd perl-perlio-gzip;
			yes y | makepkg -si --needed;
			cd ..; rm -rf perl-perlio-gzip
			popd"
		# sed is to modify sources section in PKGBUILD
		# By default it uses git:// which will fail behind proxy, so
		# redirect it to http:// source instead
		su - $SUDO_USER -c "pushd /tmp;
			git clone https://aur.archlinux.org/lcov-git.git;
			cd lcov-git;
			sed -i 's/git:/git+http:/' PKGBUILD;
			makepkg -si --needed --noconfirm;
			cd .. && rm -rf lcov-git;
			popd"
	fi
	if [[ $INSTALL_FUSE == "true" ]]; then
		# Additional dependencies for FUSE and CUSE
		pacman -Sy --needed --noconfirm fuse3
	fi
	if [[ $INSTALL_RDMA == "true" ]]; then
		# Additional dependencies for RDMA transport in NVMe over Fabrics
		if [[ -n "$http_proxy" ]]; then
			gpg_options=" --keyserver hkp://keyserver.ubuntu.com:80 --keyserver-options \"http-proxy=$http_proxy\""
		fi
		su - $SUDO_USER -c "gpg $gpg_options --recv-keys 29F0D86B9C1019B1"
		su - $SUDO_USER -c "pushd /tmp;
			git clone https://aur.archlinux.org/rdma-core.git;
			cd rdma-core;
			makepkg -si --needed --noconfirm;
			cd .. && rm -rf rdma-core;
			popd"
	fi
	if [[ $INSTALL_DOCS == "true" ]]; then
		# Additional dependencies for building docs
		pacman -Sy --needed --noconfirm doxygen graphviz
		pacman -S --noconfirm --needed gd ttf-font
		su - $SUDO_USER -c "pushd /tmp;
			git clone https://aur.archlinux.org/mscgen.git;
			cd mscgen;
			makepkg -si --needed --noconfirm;
			cd .. && rm -rf mscgen;
			popd"
	fi
else
	echo "pkgdep: unknown system type."
	exit 1
fi
