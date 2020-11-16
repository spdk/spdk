#!/usr/bin/env bash

# create_vbox.sh
#
# Creates a virtual box with vagrant in the $PWD.
#
# This script creates a subdirectory called $PWD/<distro> and copies the Vagrantfile
# into that directory before running 'vagrant up'

set -e

VAGRANT_TARGET="$PWD"

DIR="$(cd "$(dirname $0)" && pwd)"
SPDK_DIR="$(cd "${DIR}/../../" && pwd)"

# The command line help
display_help() {
	echo
	echo " Usage: ${0##*/} [-b nvme-backing-file] [-n <num-cpus>] [-s <ram-size>] [-x <http-proxy>] [-hvrldcu] <distro>"
	echo
	echo "  distro = <centos7 | centos8| ubuntu1604 | ubuntu1804 | ubuntu2004 |"
	echo "            fedora31 | fedora32 | fedora33 | freebsd11 | freebsd12 | arch | clearlinux>"
	echo
	echo "  -s <ram-size> in MB             Default: ${SPDK_VAGRANT_VMRAM}"
	echo "  -n <num-cpus> 1 to 4            Default: ${SPDK_VAGRANT_VMCPU}"
	echo "  -x <http-proxy>                 Default: \"${SPDK_VAGRANT_HTTP_PROXY}\""
	echo "  -p <provider>                   \"libvirt\" or \"virtualbox\". Default: ${SPDK_VAGRANT_PROVIDER}"
	echo "  -b <nvme-backing-file>          Emulated NVMe options."
	echo "                                  If no -b option is specified then this option defaults to emulating single"
	echo "                                  NVMe with 1 namespace and assumes path: /var/lib/libvirt/images/nvme_disk.img"
	echo "                                  -b option can be used multiple times for attaching multiple files to the VM"
	echo "                                  Parameters for -b option: <path>,<type>,<namespaces>,<cmb>"
	echo "                                  Available types: nvme, ocssd."
	echo "  -c                              Create all above disk, default 0"
	echo "  -H                              Use hugepages for allocating VM memory. Only for libvirt provider. Default: false."
	echo "  -u                              Use password authentication to the VM instead of SSH keys."
	echo "  -l                              Use a local copy of spdk, don't try to rsync from the host."
	echo "  -a                              Copy spdk/autorun.sh artifacts from VM to host system."
	echo "  -d                              Deploy a test vm by provisioning all prerequisites for spdk autotest"
	echo "  -o                              Add network interface for openstack tests"
	echo "  --qemu-emulator=<path>          Path to custom QEMU binary. Only works with libvirt provider"
	echo "  --vagrantfiles-dir=<path>       Destination directory to put Vagrantfile into."
	echo "  --package-box                   Install all dependencies for SPDK and create a local vagrant box version."
	echo " --vagrantfile=<path>             Path to a custom Vagrantfile"
	echo "  -r dry-run"
	echo "  -h help"
	echo "  -v verbose"
	echo
	echo " Examples:"
	echo
	echo "  $0 -x http://user:password@host:port fedora33"
	echo "  $0 -s 2048 -n 2 ubuntu16"
	echo "  $0 -rv freebsd"
	echo "  $0 fedora33"
	echo "  $0 -b /var/lib/libvirt/images/nvme1.img,nvme,1 fedora33"
	echo "  $0 -b /var/lib/libvirt/images/ocssd.img,ocssd fedora33"
	echo "  $0 -b /var/lib/libvirt/images/nvme5.img,nvme,5 -b /var/lib/libvirt/images/ocssd.img,ocssd fedora33"
	echo
}

# Set up vagrant proxy. Assumes git-bash on Windows
# https://stackoverflow.com/questions/19872591/how-to-use-vagrant-in-a-proxy-environment
SPDK_VAGRANT_HTTP_PROXY=""

VERBOSE=0
HELP=0
COPY_SPDK_DIR=1
COPY_SPDK_ARTIFACTS=0
DRY_RUN=0
DEPLOY_TEST_VM=0
SPDK_VAGRANT_DISTRO="distro"
SPDK_VAGRANT_VMCPU=4
SPDK_VAGRANT_VMRAM=4096
SPDK_VAGRANT_PROVIDER="virtualbox"
SPDK_QEMU_EMULATOR=""
SPDK_OPENSTACK_NETWORK=0
OPTIND=1
NVME_DISKS_TYPE=""
NVME_DISKS_NAMESPACES=""
NVME_FILE=""
NVME_AUTO_CREATE=0
VAGRANTFILE_DIR=""
VAGRANT_PASSWORD_AUTH=0
VAGRANT_PACKAGE_BOX=0
VAGRANT_HUGE_MEM=0
VAGRANTFILE=$DIR/Vagrantfile

while getopts ":b:n:s:x:p:uvcraldoHh-:" opt; do
	case "${opt}" in
		-)
			case "${OPTARG}" in
				package-box) VAGRANT_PACKAGE_BOX=1 ;;
				qemu-emulator=*) SPDK_QEMU_EMULATOR="${OPTARG#*=}" ;;
				vagrantfiles-dir=*) VAGRANTFILE_DIR="${OPTARG#*=}" ;;
				vagrantfile=*) [[ -n ${OPTARG#*=} ]] && VAGRANTFILE="${OPTARG#*=}" ;;
				*) echo "Invalid argument '$OPTARG'" ;;
			esac
			;;
		x)
			http_proxy=$OPTARG
			https_proxy=$http_proxy
			SPDK_VAGRANT_HTTP_PROXY="${http_proxy}"
			;;
		n)
			SPDK_VAGRANT_VMCPU=$OPTARG
			;;
		s)
			SPDK_VAGRANT_VMRAM=$OPTARG
			;;
		p)
			SPDK_VAGRANT_PROVIDER=$OPTARG
			;;
		v)
			VERBOSE=1
			;;
		c)
			NVME_AUTO_CREATE=1
			;;
		r)
			DRY_RUN=1
			;;
		h)
			display_help >&2
			exit 0
			;;
		a)
			COPY_SPDK_ARTIFACTS=1
			;;
		l)
			COPY_SPDK_DIR=0
			;;
		d)
			DEPLOY_TEST_VM=1
			;;
		o)
			SPDK_OPENSTACK_NETWORK=1
			;;
		b)
			NVME_FILE+="${OPTARG#*=} "
			;;
		u)
			VAGRANT_PASSWORD_AUTH=1
			;;
		H)
			VAGRANT_HUGE_MEM=1
			;;
		*)
			echo "  Invalid argument: -$OPTARG" >&2
			echo "  Try: \"$0 -h\"" >&2
			exit 1
			;;
	esac
done

shift "$((OPTIND - 1))" # Discard the options and sentinel --

SPDK_VAGRANT_DISTRO="$*"

case "${SPDK_VAGRANT_DISTRO}" in
	centos7)
		export SPDK_VAGRANT_DISTRO
		;;
	centos8)
		export SPDK_VAGRANT_DISTRO
		;;
	ubuntu1604)
		export SPDK_VAGRANT_DISTRO
		;;
	ubuntu1804)
		export SPDK_VAGRANT_DISTRO
		;;
	ubuntu2004)
		export SPDK_VAGRANT_DISTRO
		;;
	fedora31)
		export SPDK_VAGRANT_DISTRO
		;;
	fedora32)
		export SPDK_VAGRANT_DISTRO
		;;
	fedora33)
		export SPDK_VAGRANT_DISTRO
		;;
	freebsd11)
		export SPDK_VAGRANT_DISTRO
		;;
	freebsd12)
		export SPDK_VAGRANT_DISTRO
		;;
	arch)
		export SPDK_VAGRANT_DISTRO
		;;
	clearlinux)
		export SPDK_VAGRANT_DISTRO
		;;
	*)
		echo "  Invalid argument \"${SPDK_VAGRANT_DISTRO}\""
		echo "  Try: \"$0 -h\"" >&2
		exit 1
		;;
esac

if ! echo "$SPDK_VAGRANT_DISTRO" | grep -q fedora && [ $DEPLOY_TEST_VM -eq 1 ]; then
	echo "Warning: Test machine deployment is only available on fedora distros. Disabling it for this build"
	DEPLOY_TEST_VM=0
fi
if [ -z "$NVME_FILE" ]; then
	TMP="/var/lib/libvirt/images/nvme_disk.img"
	NVME_DISKS_TYPE="nvme"
else
	TMP=""
	for args in $NVME_FILE; do
		while IFS=, read -r path type namespace cmb; do
			TMP+="$path,"
			if [ -z "$type" ]; then
				type="nvme"
			fi
			if [[ -n $cmb ]]; then
				NVME_CMB=${NVME_CMB:+$NVME_CMB,}true
			fi
			NVME_DISKS_TYPE+="$type,"
			if [ -z "$namespace" ] && [ -n "$SPDK_QEMU_EMULATOR" ]; then
				namespace="1"
			fi
			NVME_DISKS_NAMESPACES+="$namespace,"
			if [ ${NVME_AUTO_CREATE} = 1 ]; then
				$SPDK_DIR/scripts/vagrant/create_nvme_img.sh -t $type -n $path
			fi
		done <<< $args
	done
fi
NVME_FILE=$TMP

if [ ${VERBOSE} = 1 ]; then
	echo
	echo DIR=${DIR}
	echo SPDK_DIR=${SPDK_DIR}
	echo VAGRANT_TARGET=${VAGRANT_TARGET}
	echo HELP=$HELP
	echo DRY_RUN=$DRY_RUN
	echo NVME_FILE=$NVME_FILE
	echo NVME_DISKS_TYPE=$NVME_DISKS_TYPE
	echo NVME_AUTO_CREATE=$NVME_AUTO_CREATE
	echo NVME_DISKS_NAMESPACES=$NVME_DISKS_NAMESPACES
	echo NVME_CMB=$NVME_CMB
	echo SPDK_VAGRANT_DISTRO=$SPDK_VAGRANT_DISTRO
	echo SPDK_VAGRANT_VMCPU=$SPDK_VAGRANT_VMCPU
	echo SPDK_VAGRANT_VMRAM=$SPDK_VAGRANT_VMRAM
	echo SPDK_VAGRANT_PROVIDER=$SPDK_VAGRANT_PROVIDER
	echo SPDK_VAGRANT_HTTP_PROXY=$SPDK_VAGRANT_HTTP_PROXY
	echo SPDK_QEMU_EMULATOR=$SPDK_QEMU_EMULATOR
	echo SPDK_OPENSTACK_NETWORK=$SPDK_OPENSTACK_NETWORK
	echo VAGRANT_PACKAGE_BOX=$VAGRANT_PACKAGE_BOX
	echo VAGRANTFILE=$VAGRANTFILE
	echo
fi

export SPDK_VAGRANT_HTTP_PROXY
export SPDK_VAGRANT_VMCPU
export SPDK_VAGRANT_VMRAM
export SPDK_DIR
export SPDK_OPENSTACK_NETWORK
export COPY_SPDK_DIR
export COPY_SPDK_ARTIFACTS
export DEPLOY_TEST_VM
export NVME_CMB
export NVME_DISKS_TYPE
export NVME_DISKS_NAMESPACES
export NVME_FILE
export VAGRANT_PASSWORD_AUTH
export VAGRANT_HUGE_MEM

if [ -n "$SPDK_VAGRANT_PROVIDER" ]; then
	provider="--provider=${SPDK_VAGRANT_PROVIDER}"
fi

if [ -n "$SPDK_VAGRANT_PROVIDER" ]; then
	export SPDK_VAGRANT_PROVIDER
fi

if [ -n "$SPDK_QEMU_EMULATOR" ] && [ "$SPDK_VAGRANT_PROVIDER" == "libvirt" ]; then
	export SPDK_QEMU_EMULATOR
fi

if [ ${DRY_RUN} = 1 ]; then
	echo "Environemnt Variables"
	printenv SPDK_VAGRANT_DISTRO
	printenv SPDK_VAGRANT_VMRAM
	printenv SPDK_VAGRANT_VMCPU
	printenv SPDK_VAGRANT_PROVIDER
	printenv SPDK_VAGRANT_HTTP_PROXY
	printenv SPDK_QEMU_EMULATOR
	printenv NVME_DISKS_TYPE
	printenv NVME_AUTO_CREATE
	printenv NVME_DISKS_NAMESPACES
	printenv NVME_FILE
	printenv SPDK_DIR
	printenv VAGRANT_HUGE_MEM
	printenv VAGRANTFILE
fi
if [ -z "$VAGRANTFILE_DIR" ]; then
	VAGRANTFILE_DIR="${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}-${SPDK_VAGRANT_PROVIDER}"
	export VAGRANTFILE_DIR
fi

if [ -d "${VAGRANTFILE_DIR}" ]; then
	echo "Error: ${VAGRANTFILE_DIR} already exists!"
	exit 1
fi

if [[ ! -f $VAGRANTFILE ]]; then
	echo "$VAGRANTFILE is not a regular file!"
	exit 1
fi

if [ ${DRY_RUN} != 1 ]; then
	mkdir -vp "${VAGRANTFILE_DIR}"
	ln -s "$VAGRANTFILE" "${VAGRANTFILE_DIR}/Vagrantfile"
	pushd "${VAGRANTFILE_DIR}"
	if [ -n "${http_proxy}" ]; then
		export http_proxy
		export https_proxy
		if echo "$SPDK_VAGRANT_DISTRO" | grep -q freebsd; then
			cat > ~/vagrant_pkg.conf << EOF
pkg_env: {
http_proxy: ${http_proxy}
}
EOF
		fi
	fi
	mkdir -p "${VAGRANTFILE_DIR}/output"
	vagrant up $provider
	if [ ${VAGRANT_PACKAGE_BOX} == 1 ]; then
		vagrant ssh -c 'sudo spdk_repo/spdk/scripts/vagrant/update.sh'
		vagrant halt
		vagrant package --output spdk_${SPDK_VAGRANT_DISTRO}.box
		vagrant box add spdk/${SPDK_VAGRANT_DISTRO} spdk_${SPDK_VAGRANT_DISTRO}.box \
			&& rm spdk_${SPDK_VAGRANT_DISTRO}.box
		vagrant destroy
	fi
	echo ""
	echo "  SUCCESS!"
	echo ""
	echo "  cd to ${VAGRANTFILE_DIR} and type \"vagrant ssh\" to use."
	echo "  Use vagrant \"suspend\" and vagrant \"resume\" to stop and start."
	echo "  Use vagrant \"destroy\" followed by \"rm -rf ${VAGRANTFILE_DIR}\" to destroy all trace of vm."
	echo ""
fi
