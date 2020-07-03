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

sudo() {
	"$(type -P sudo)" -E "$@"
}

set -e

VM_SETUP_PATH=$(readlink -f ${BASH_SOURCE%/*})

UPGRADE=false
INSTALL=false
CONF="rocksdb,fio,flamegraph,tsocks,qemu,libiscsi,nvmecli,qat,spdk,refspdk"

if [[ -e /etc/os-release ]]; then
	source /etc/os-release
fi

if [ $(uname -s) == "FreeBSD" ]; then
	OSID="freebsd"
	OSVERSION=$(freebsd-version | cut -d. -f1)
else
	OSID=$(source /etc/os-release && echo $ID)
	OSVERSION=$(source /etc/os-release && echo $VERSION_ID)
fi

function usage() {
	echo "This script is intended to automate the environment setup for a linux virtual machine."
	echo "Please run this script as your regular user. The script will make calls to sudo as needed."
	echo ""
	echo "./vm_setup.sh"
	echo "  -h --help"
	echo "  -u --upgrade Run $package_manager upgrade"
	echo "  -i --install-deps Install $package_manager based dependencies"
	echo "  -t --test-conf List of test configurations to enable (${CONF})"
	echo "  -c --conf-path Path to configuration file"
	echo "  -d --dir-git Path to where git sources should be saved"
	echo "  -s --disable-tsocks Disable use of tsocks"
	exit 0
}

vmsetupdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$vmsetupdir/../../../")

managers=("$vmsetupdir/pkgdep/"*)
# Get package manager #
if hash dnf &> /dev/null; then
	source "$vmsetupdir/pkgdep/dnf"
elif hash yum &> /dev/null; then
	source "$vmsetupdir/pkgdep/yum"
elif hash apt-get &> /dev/null; then
	source "$vmsetupdir/pkgdep/apt-get"
elif hash pacman &> /dev/null; then
	source "$vmsetupdir/pkgdep/pacman"
elif hash pkg &> /dev/null; then
	source "$vmsetupdir/pkgdep/pkg"
elif hash swupd &> /dev/null; then
	source "$vmsetupdir/pkgdep/swupd"
else
	package_manager="undefined"
fi

# Parse input arguments #
while getopts 'd:siuht:c:-:' optchar; do
	case "$optchar" in
		-)
			case "$OPTARG" in
				help) usage ;;
				upgrade) UPGRADE=true ;;
				install-deps) INSTALL=true ;;
				test-conf=*) CONF="${OPTARG#*=}" ;;
				conf-path=*) CONF_PATH="${OPTARG#*=}" ;;
				dir-git=*) GIT_REPOS="${OPTARG#*=}" ;;
				disable-tsocks) NO_TSOCKS=true ;;
				*)
					echo "Invalid argument '$OPTARG'"
					usage
					;;
			esac
			;;
		h) usage ;;
		u) UPGRADE=true ;;
		i) INSTALL=true ;;
		t) CONF="$OPTARG" ;;
		c) CONF_PATH="$OPTARG" ;;
		d) GIT_REPOS="$OPTARG" ;;
		s) NO_TSOCKS=true ;;
		*)
			echo "Invalid argument '$OPTARG'"
			usage
			;;
	esac
done

if [[ "$package_manager" == "undefined" ]]; then
	echo "Supported package manager not found. Script supports:"
	printf ' * %s\n' "${managers[@]##*/}"
	exit 1
fi

if [ -n "$CONF_PATH" ]; then
	if [ ! -f "$CONF_PATH" ]; then
		echo Configuration file does not exist: "$CONF_PATH"
		exit 1
	else
		source "$CONF_PATH"
	fi
fi

if $UPGRADE; then
	upgrade
fi

if $INSTALL; then
	sudo "$rootdir/scripts/pkgdep.sh" --all
	pre_install
	install "${packages[@]}"
fi

source "$vmsetupdir/pkgdep/git"

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
