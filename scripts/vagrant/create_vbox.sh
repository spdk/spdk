#!/usr/bin/env bash

# create_vbox.sh
#
# Creates a virtual box with vagrant in the $PWD.
#
# This script creates a subdirectory called $PWD/<distro> and copies the Vagrantfile
# into that directory before running 'vagrant up'

VAGRANT_TARGET="$PWD"

DIR="$( cd "$( dirname $0 )" && pwd )"
SPDK_DIR="$( cd "${DIR}/../../" && pwd )"

# The command line help
display_help() {
	echo
	echo " Usage: ${0##*/} [-b nvme-backing-file] [-n <num-cpus>] [-s <ram-size>] [-x <http-proxy>] [-hvrld] <distro>"
	echo
	echo "  distro = <centos7 | ubuntu16 | ubuntu18 | fedora28 | fedora29 | fedora 30 | freebsd11> "
	echo
	echo "  -b <nvme-backing-file>          nvme file path with name"
	echo "                                  type of emulated nvme disk"
	echo "                                  usage: type <number_of_namespaces> types available: ocssd, nvme"
	echo "                                  If no -b option is specified then this option defaults to emulating single"
	echo "                                  NVMe with 1 namespace and assumes path: /var/lib/libvirt/images/nvme_disk.img"
	echo "  -s <ram-size> in kb             default: ${SPDK_VAGRANT_VMRAM}"
	echo "  -n <num-cpus> 1 to 4            default: ${SPDK_VAGRANT_VMCPU}"
	echo "  -x <http-proxy>                 default: \"${SPDK_VAGRANT_HTTP_PROXY}\""
	echo "  -p <provider>                   libvirt or virtualbox"
	echo "  --vhost-host-dir=<path>         directory path with vhost test dependencies"
	echo "                                  (test VM qcow image, fio binary, ssh keys)"
	echo "  --vhost-vm-dir=<path>           directory where to put vhost dependencies in VM"
	echo "  --qemu-emulator=<path>          directory path with emulator, default: ${SPDK_QEMU_EMULATOR}"
	echo "  --vagrantfiles-dir=<path>       directory to put vagrantfile"
	echo "  -r dry-run"
	echo "  -l use a local copy of spdk, don't try to rsync from the host."
	echo "  -d deploy a test vm by provisioning all prerequisites for spdk autotest"
	echo "  -h help"
	echo "  -v verbose"
	echo
	echo " Examples:"
	echo
	echo "  $0 -x http://user:password@host:port fedora28"
	echo "  $0 -s 2048 -n 2 ubuntu16"
	echo "  $0 -rv freebsd"
	echo "  $0 fedora28"
	echo "  $0 -b /var/lib/libvirt/images/nvme1.img,nvme,1 fedora30"
	echo "  $0 -b /var/lib/libvirt/images/ocssd.img,ocssd fedora30"
	echo "  $0 -b /var/lib/libvirt/images/nvme5.img,nvme,5 -b /var/lib/libvirt/images/ocssd.img,ocssd fedora30"
	echo
}

# Set up vagrant proxy. Assumes git-bash on Windows
# https://stackoverflow.com/questions/19872591/how-to-use-vagrant-in-a-proxy-environment
SPDK_VAGRANT_HTTP_PROXY=""

VERBOSE=0
HELP=0
COPY_SPDK_DIR=1
DRY_RUN=0
DEPLOY_TEST_VM=0
SPDK_VAGRANT_DISTRO="distro"
SPDK_VAGRANT_VMCPU=4
SPDK_VAGRANT_VMRAM=4096
SPDK_VAGRANT_PROVIDER="virtualbox"
SPDK_QEMU_EMULATOR=""
OPTIND=1
NVME_DISKS_TYPE=""
NVME_DISKS_NAMESPACES=""
NVME_FILE=""
VAGRANTFILE_DIR=""

while getopts ":b:n:s:x:p:vrldh-:" opt; do
	case "${opt}" in
		-)
		case "${OPTARG}" in
			vhost-host-dir=*) VHOST_HOST_DIR="${OPTARG#*=}" ;;
			vhost-vm-dir=*) VHOST_VM_DIR="${OPTARG#*=}" ;;
			qemu-emulator=*) SPDK_QEMU_EMULATOR="${OPTARG#*=}" ;;
			vagrantfiles-dir=*) VAGRANTFILE_DIR="${OPTARG#*=}" ;;
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
		r)
			DRY_RUN=1
		;;
		h)
			display_help >&2
			exit 0
		;;
		l)
			COPY_SPDK_DIR=0
		;;
		d)
			DEPLOY_TEST_VM=1
		;;
		b)
			NVME_FILE+="${OPTARG#*=} "
		;;
		*)
			echo "  Invalid argument: -$OPTARG" >&2
			echo "  Try: \"$0 -h\"" >&2
			exit 1
		;;
	esac
done

shift "$((OPTIND-1))"   # Discard the options and sentinel --

SPDK_VAGRANT_DISTRO=( "$@" )

case "$SPDK_VAGRANT_DISTRO" in
	centos7)
		export SPDK_VAGRANT_DISTRO
	;;
	ubuntu16)
		export SPDK_VAGRANT_DISTRO
	;;
	ubuntu18)
		export SPDK_VAGRANT_DISTRO
	;;
	fedora28)
		export SPDK_VAGRANT_DISTRO
	;;
	fedora29)
		export SPDK_VAGRANT_DISTRO
	;;
	fedora30)
		export SPDK_VAGRANT_DISTRO
	;;
	freebsd11)
		export SPDK_VAGRANT_DISTRO
	;;
	arch-linux)
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
	NVME_DISKS_NAMESPACES="1"
else
	TMP=""
	for args in $NVME_FILE; do
		while IFS=, read -r path type namespace; do
			TMP+="$path,";
			if [ -z "$type" ]; then
				type="nvme"
			fi
			NVME_DISKS_TYPE+="$type,";
			if [ -z "$namespace" ]; then
				namespace="1"
			fi
			NVME_DISKS_NAMESPACES+="$namespace,";
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
	echo NVME_DISKS_NAMESPACES=$NVME_DISKS_NAMESPACES
	echo SPDK_VAGRANT_DISTRO=$SPDK_VAGRANT_DISTRO
	echo SPDK_VAGRANT_VMCPU=$SPDK_VAGRANT_VMCPU
	echo SPDK_VAGRANT_VMRAM=$SPDK_VAGRANT_VMRAM
	echo SPDK_VAGRANT_PROVIDER=$SPDK_VAGRANT_PROVIDER
	echo SPDK_VAGRANT_HTTP_PROXY=$SPDK_VAGRANT_HTTP_PROXY
	echo VHOST_HOST_DIR=$VHOST_HOST_DIR
	echo VHOST_VM_DIR=$VHOST_VM_DIR
	echo SPDK_QEMU_EMULATOR=$SPDK_QEMU_EMULATOR
	echo
fi

export SPDK_VAGRANT_HTTP_PROXY
export SPDK_VAGRANT_VMCPU
export SPDK_VAGRANT_VMRAM
export SPDK_DIR
export COPY_SPDK_DIR
export DEPLOY_TEST_VM
export NVME_DISKS_TYPE
export NVME_DISKS_NAMESPACES
export NVME_FILE

if [ -n "$SPDK_VAGRANT_PROVIDER" ]; then
    provider="--provider=${SPDK_VAGRANT_PROVIDER}"
fi

if [ -n "$VHOST_HOST_DIR" ]; then
    export VHOST_HOST_DIR
fi

if [ -n "$VHOST_VM_DIR" ]; then
    export VHOST_VM_DIR
fi

if [ -n "$SPDK_VAGRANT_PROVIDER" ]; then
    export SPDK_VAGRANT_PROVIDER
fi

if [ -n "$SPDK_QEMU_EMULATOR" ] && [ "$SPDK_VAGRANT_PROVIDER" == "libvirt"  ]; then
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
	printenv NVME_DISKS_NAMESPACES
	printenv NVME_FILE
	printenv SPDK_DIR
fi
if [ -z "$VAGRANTFILE_DIR" ]; then
	VAGRANTFILE_DIR="${VAGRANT_TARGET}/${SPDK_VAGRANT_DISTRO}-${SPDK_VAGRANT_PROVIDER}"
fi

if [ -d "${VAGRANTFILE_DIR}" ]; then
	echo "Error: ${VAGRANTFILE_DIR} already exists!"
	exit 1
fi

if [ ${DRY_RUN} != 1 ]; then
	mkdir -vp "${VAGRANTFILE_DIR}"
	cp ${DIR}/Vagrantfile ${VAGRANTFILE_DIR}
	pushd "${VAGRANTFILE_DIR}"
	if [ -n "${http_proxy}" ]; then
		export http_proxy
		export https_proxy
		if vagrant plugin list | grep -q vagrant-proxyconf; then
			echo "vagrant-proxyconf already installed... skipping"
		else
			vagrant plugin install vagrant-proxyconf
		fi
		if echo "$SPDK_VAGRANT_DISTRO" | grep -q freebsd; then
			cat >~/vagrant_pkg.conf <<EOF
pkg_env: {
http_proxy: ${http_proxy}
}
EOF
		fi
	fi
	vagrant up $provider
	echo ""
	echo "  SUCCESS!"
	echo ""
	echo "  cd to ${VAGRANTFILE_DIR} and type \"vagrant ssh\" to use."
	echo "  Use vagrant \"suspend\" and vagrant \"resume\" to stop and start."
	echo "  Use vagrant \"destroy\" followed by \"rm -rf ${VAGRANTFILE_DIR}\" to destroy all trace of vm."
	echo ""
fi
