#!/usr/bin/env bash

set -e

scriptdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $scriptdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

vagrantdir=$rootdir/scripts/vagrant
vagrant_vm=$rootdir/../fedora27-virtualbox/

trap "clean_vagrant; exit 1" SIGINT SIGTERM EXIT

function clean_vagrant {
	sleep 2
	cd $vagrant_vm; vagrant destroy -f
	cd $rootdir; rm -rf $vagrant_vm
}

set -x
timing_enter vagrant_create_vm

if [ -s /etc/redhat-release ]; then
	if ! $(yum list -q installed VirtualBox-5.2 > /dev/null); then
		echo "Installing VirtualBox"
		sudo yum install -y VirtualBox-5.2
	fi

	if ! $(yum list -q installed vagrant > /dev/null); then
		echo "Installing vagrant"
		sudo yum install -y vagrant
	fi
elif [ -f /etc/debian_version ]; then
	if ! $(yum list -q installed VirtualBox-5.2 > /dev/null); then
		echo "Installing VirtualBox"
		sudo apt-get install -y virtualbox-5.2
	fi

	if ! $(yum list -q installed vagrant > /dev/null); then
		echo "Installing vagrant"
		sudo apt-get install -y vagrant
	fi
fi

if ! VBoxManage list extpacks | grep -Fq "Oracle VM VirtualBox Extension Pack"; then
	echo "Installing Vbox extension pack"
	VboxVersion=$(wget -qO - http://download.virtualbox.org/virtualbox/LATEST.TXT)
	wget "http://download.virtualbox.org/virtualbox/${VboxVersion}/Oracle_VM_VirtualBox_Extension_Pack-${VboxVersion}.vbox-extpack"
	yes | VBoxManage extpack install --replace Oracle_VM_VirtualBox_Extension_Pack-${VboxVersion}.vbox-extpack
fi

if ! vagrant plugin list | grep -Fq "vagrant-proxyconf"; then
	vagrant plugin install vagrant-proxyconf
fi

cd $rootdir/..
sudo $vagrantdir/create_nvme_img.sh -s 2G
$vagrantdir/create_vbox.sh -s 8192 fedora27

cd $vagrant_vm

vagrant ssh -c "sudo spdk_repo/spdk/scripts/vagrant/update.sh"

if ! vagrant ssh -c 'lsblk | grep -Fq "nvme0n1"'; then
	echo "Nvme device not fount on created VM"
	exit 1
fi

vagrant ssh -c "cd spdk_repo/spdk; ./configure; make clean; make -j4"
vagrant ssh -c "sudo ./spdk_repo/spdk/scripts/setup.sh"
vagrant ssh -c "sudo ./spdk_repo/spdk/examples/nvme/hello_world/hello_world"

clean_vagrant

timing_exit vagrant_create_vm
