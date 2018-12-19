#!/usr/bin/env bash

set -e

scriptdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $scriptdir/../..)

VM_MEMORY=10240
VM_CORES=16
VM_DISRTO=fedora28
INSTALL=false
RUN_AUTOTEST=false
VM_PROXY=""

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Automated script for testing creating vagrant VM with SPKD."
	echo "Usage: ${0##*/} [-h|--help] [-i|--install] [-n <num-cpus>] [-s <ram-size>] [-t|--test] [-x <http_proxy>] [-d|--distro=<linux-distro>]"
	echo "-h, --help            Print help and exit"
	echo "-i  --install         Install Vagrant, VirtualBox and plugins"
	echo "-t  --test            Run spdk autotest otherwise check only 'hello_world' application."
	echo "                      Autotest parts can be selected in autorun-pdk.conf file."
	echo "-s                    VM memory in MiB. Minimum of 8192MiB is required for autotest. [default=$VM_MEMORY]"
	echo "-n                    VM cores [default=$VM_CORES]"
	echo "-x                    VM http proxy [default=$VM_PROXY]"
	echo "-d  --distro          VM linux distro, available distros are: centos7, ubuntu16, ubuntu18,"
	echo "                      fedora27, fedora28 ,freebsd11 [default=$VM_DISRTO]"
}

while getopts ':n:s:d:t:x:h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 && exit 0 ;;
			distro*) VM_DISTO=$OPTARG ;;
			install) INSTALL=true ;;
			*) usage $0 echo "Invalid argument '$OPTARG'" && exit 1 ;;
		esac
		;;
		h) usage $0 && exit 0 ;;
		i) INSTALL=true ;;
		t) RUN_AUTOTEST=true ;;
		s) VM_MEMORY=$OPTARG ;;
		n) VM_CORES=$OPTARG ;;
		d) VM_DISTO=$OPTARG ;;
		x) VM_PROXY=$OPTARG ;;
		*) usage $0 "Invalid argument '$optchar'" && exit 1 ;;
	esac
done

source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh
vagrantdir=$rootdir/scripts/vagrant
vagrant_vm=$rootdir/../$VM_DISRTO-virtualbox/

trap "clean_vagrant; exit 1" SIGINT SIGTERM EXIT

function clean_vagrant {
	cd $vagrant_vm; vagrant destroy -f
	cd $rootdir; rm -rf $vagrant_vm
}

set -x
timing_enter vagrant_create_vm
timing_enter vagrant_install
if $INSTALL; then
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
fi

timing_exit vagrant_install

timing_enter setup_vagrant_vm
cd $rootdir/..
sudo $vagrantdir/create_nvme_img.sh -s 2G
$vagrantdir/create_vbox.sh -v -s $VM_MEMORY -n $VM_CORES -x $VM_DISTO
cd $vagrant_vm

if ! vagrant ssh -c 'lsblk | grep -Fq "nvme0n1"'; then
	echo "Nvme device not fount on created VM"
	exit 1
fi

timing_enter vagrant_vm_update
vagrant ssh -c "sudo spdk_repo/spdk/scripts/vagrant/update.sh"
timing_exit vagrant_vm_update
timing_exit vagrant_setup_vm

if $RUN_AUTOTEST; then
	timing_enter vagrant_run_autotest
	vagrant ssh -c "cd spdk_repo/spdk/intel-ipsec-mb; make -j$VM_CORES; sudo make install"
	vagrant ssh -c "cp spdk_repo/spdk/test/vagrant/autorun-spdk.conf ~/"
	vagrant ssh -c "sudo ./spdk_repo/spdk/autorun.sh;"
	timing_exit vagrant_run_autotest
else
	timing_enter vagrant_vm_build_spdk
	vagrant ssh -c "cd spdk_repo/spdk; ./configure; make clean; make -j$VM_CORES"
	vagrant ssh -c "sudo ./spdk_repo/spdk/scripts/setup.sh"
	vagrant ssh -c "sudo ./spdk_repo/spdk/examples/nvme/hello_world/hello_world"
	timing_exit vagrant_vm_build_spdk
fi

clean_vagrant
timing_exit vagrant_create_vm
