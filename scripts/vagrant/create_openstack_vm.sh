#!/usr/bin/env bash

# create_vhost_vm.sh
#
# Creates a virtual machine image used as a dependency for running vhost tests

set -e

testdir=$(readlink -f $(dirname $0))
SPDK_DIR=$(readlink -f $testdir/../..)
VAGRANT_TARGET="$PWD"
VAGRANT_DISTRO="ubuntu18"

# The command line help
display_help() {
	echo
	echo " Usage:"
	echo
	echo "  --http-proxy    Define proxy to use for vm"
	echo "                  Example --http-proxy=$PROXY"
	echo "  -h              Print help"
	echo
}

while getopts ":h-:" opt; do
	case "${opt}" in
		-)
		case "${OPTARG}" in
			http-proxy=*)
				http_proxy=$OPTARG
				https_proxy=$http_proxy
			;;
			*)
				echo "  Invalid argument -$OPTARG" >&2
				echo "  Try \"$0 -h\"" >&2
				exit 1
				;;
		esac
		;;
		h)
			display_help >&2
			exit 0
		;;
		*)
			echo "  Invalid argument: -$OPTARG" >&2
			echo "  Try: \"$0 -h\"" >&2
			exit 1
		;;
	esac
done
export SPDK_DIR
export SPDK_VAGRANT_VMRAM=8192
export SPDK_VAGRANT_VMCPU=10

mkdir -vp "${VAGRANT_TARGET}/${VAGRANT_DISTRO}"
cp ${testdir}/Vagrantfile_openstack_vm ${VAGRANT_TARGET}/${VAGRANT_DISTRO}/Vagrantfile

pushd "${VAGRANT_TARGET}/${VAGRANT_DISTRO}"
if [ ! -z "${http_proxy}" ]; then
	export http_proxy
	export https_proxy
	if vagrant plugin list | grep -q vagrant-proxyconf; then
		echo "vagrant-proxyconf already installed... skipping"
	else
		vagrant plugin install vagrant-proxyconf
	fi
fi
VBoxManage setproperty machinefolder "${VAGRANT_TARGET}/${VAGRANT_DISTRO}"
vagrant up
vagrant halt
VBoxManage setproperty machinefolder default

# Convert Vbox .vmkd image to qcow2
vmdk_img=$(find ${VAGRANT_TARGET}/${VAGRANT_DISTRO} -name "*.vmdk")
qemu-img convert -f vmdk -O qcow2 ${vmdk_img} ${VAGRANT_TARGET}/${VAGRANT_DISTRO}/openstack_vm_image.qcow2

echo ""
echo "  SUCCESS!"
echo ""
