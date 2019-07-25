#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
SPDK_DIR=$(readlink -f $testdir/../..)
VAGRANT_TARGET="$PWD"
VAGRANT_DISTRO="ubuntu18"

export SPDK_DIR
export SPDK_VAGRANT_VMRAM=8192
export SPDK_VAGRANT_VMCPU=10

mkdir -vp "${VAGRANT_TARGET}/${VAGRANT_DISTRO}"
cp "${testdir}/Vagrantfile_openstack_vm" "${VAGRANT_TARGET}/${VAGRANT_DISTRO}/Vagrantfile"

pushd "${VAGRANT_TARGET}/${VAGRANT_DISTRO}"
if [ -n "${http_proxy}" ]; then
	export http_proxy
fi

VBoxManage setproperty machinefolder "${VAGRANT_TARGET}/${VAGRANT_DISTRO}"
vagrant up
vagrant halt
VBoxManage setproperty machinefolder default

# Convert Vbox .vmdk image to qcow2
vmdk_img=$(find ${VAGRANT_TARGET}/${VAGRANT_DISTRO} -name "ubuntu-18.04-amd64-disk001.vmdk")
qemu-img convert -f vmdk -O qcow2 ${vmdk_img} ${VAGRANT_TARGET}/${VAGRANT_DISTRO}/openstack_vm_image.qcow2

echo ""
echo "  SUCCESS!"
echo ""
