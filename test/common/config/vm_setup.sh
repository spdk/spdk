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
shopt -s extglob

VM_SETUP_PATH=$(readlink -f ${BASH_SOURCE%/*})

UPGRADE=false
INSTALL=false
CONF="rocksdb,fio,flamegraph,tsocks,qemu,libiscsi,nvmecli,qat,spdk,refspdk,vagrant"
package_manager=

function pre_install() { :; }
function install() { :; }
function upgrade() { :; }

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
	exit ${1:-0}
}

function error() {
	printf "%s\n\n" "$1" >&2
	usage 1
}

function set_os_id_version() {
	if [[ $(uname -s) == FreeBSD ]] && ! pkg info -q etc_os-release; then
		echo "Please install 'etc_os-release' package" >&2
		echo "pkg install -y etc_os-release" >&2
		exit 2
	fi

	if [[ -f /etc/os-release ]]; then
		source /etc/os-release
	elif [[ -f /usr/local/etc/os-release ]]; then
		# On FreeBSD file is located under /usr/local if etc_os-release package is installed
		source /usr/local/etc/os-release
	elif [[ $(uname -s) == FreeBSD ]]; then
		ID=freebsd
		VERSION_ID=$(freebsd-version)
		VERSION_ID=${VERSION_ID//.*/}
	else
		echo "File os-release not found" >&2
		exit 3
	fi

	OSID=$ID
	OSVERSION=$VERSION_ID

	echo "OS-ID: $OSID | OS-Version: $OSVERSION"
}

function detect_package_manager() {
	local manager_scripts
	manager_scripts=("$vmsetupdir/pkgdep/"!(git))

	local package_manager_lib
	for package_manager_lib in "${manager_scripts[@]}"; do
		package_manager=${package_manager_lib##*/}
		if hash "${package_manager}" &> /dev/null; then
			source "${package_manager_lib}"
			return
		fi
	done

	package_manager="undefined"
}

vmsetupdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$vmsetupdir/../../../")
source "$rootdir/scripts/common.sh"

set_os_id_version
detect_package_manager

if [[ -e $vmsetupdir/pkgdep/os/$OSID ]]; then
	source "$vmsetupdir/pkgdep/os/$OSID"
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
				*) error "Invalid argument '$OPTARG'" ;;
			esac
			;;
		h) usage ;;
		u) UPGRADE=true ;;
		i) INSTALL=true ;;
		t) CONF="$OPTARG" ;;
		c) CONF_PATH="$OPTARG" ;;
		d) GIT_REPOS="$OPTARG" ;;
		s) NO_TSOCKS=true ;;
		*) error "Invalid argument '$OPTARG'" ;;
	esac
done

if [[ $package_manager == undefined ]]; then
	echo "Supported package manager not found. Script supports:"
	printf " * %s\n" "${manager_scripts[@]##*/}"
	exit 1
fi

if [[ $package_manager == apt-get ]]; then
	export DEBIAN_FRONTEND=noninteractive
fi

if [[ -n $CONF_PATH ]]; then
	if [[ -f $CONF_PATH ]]; then
		source "$CONF_PATH"
	else
		error "Configuration file does not exist: '$CONF_PATH'"
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
if [[ ! -e ~/autorun-spdk.conf ]]; then
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
SPDK_TEST_NVME_CUSE=1
SPDK_TEST_BLOBFS=1
SPDK_TEST_URING=1
SPDK_RUN_ASAN=1
SPDK_RUN_UBSAN=1
# doesn't work on vm
SPDK_TEST_IOAT=0
# requires some extra configuration. see TEST_ENV_SETUP_README
SPDK_TEST_VHOST=0
SPDK_TEST_VHOST_INIT=0

EOF
fi
