#!/usr/bin/env bash

set -e

scriptdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $scriptdir/../..)

VM_MEMORY=10240
VM_CORES=16
VM_DISTRO=fedora28
VM_PROVIDER=virtualbox
RUN_AUTOTEST=false
VM_PROXY=""

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Automated script for testing creating vagrant VM with SPKD."
	echo "To run this test, vagrant, virtualbox with virtualbox expansion pack"
	echo "needs to be installed and configured on the system."
	echo "Usage: ${0##*/} [-h|--help] [-n <num-cpus>] [-s <ram-size>] \
[-t|--test] [-x <http_proxy>] [-d|--distro=<linux-distro>]"
	echo "-h, --help            Print help and exit"
	echo "-t  --test            Run spdk autotest otherwise check only 'hello_world' application."
	echo "                      Autotest parts can be selected in autorun-pdk.conf file."
	echo "-s                    VM memory in MiB. Minimum of 8192MiB is required for autotest. [default=$VM_MEMORY]"
	echo "-n                    VM cores [default=$VM_CORES]"
	echo "-x                    VM http proxy [default=$VM_PROXY]"
	echo "-p                    VM provider, libvirt or virtualbox [default=$VM_PROVIDER]"
	echo "-d  --distro          VM linux distro, available distros are: centos7, ubuntu16, ubuntu18,"
	echo "                      fedora27, fedora28 ,freebsd11 [default=$VM_DISTRO]"
}

while getopts "n:s:d:p:txh-:" optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 && exit 0 ;;
			distro=*) VM_DISTRO="${OPTARG#*=}" ;;
			test) RUN_AUTOTEST=true ;;
			*) usage $0 echo "Invalid argument '$OPTARG'" && exit 1 ;;
		esac
		;;
		h) usage $0 && exit 0 ;;
		t) RUN_AUTOTEST=true ;;
		s) VM_MEMORY=$OPTARG ;;
		n) VM_CORES=$OPTARG ;;
		d) VM_DISTRO=$OPTARG;;
		x) VM_PROXY="-x $OPTARG" ;;
		p) VM_PROVIDER=$OPTARG;;
		*) usage $0 "Invalid argument '$optchar'" && exit 1 ;;
	esac
done

source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh
vagrantdir=$rootdir/scripts/vagrant

if [[ "$VM_PROVIDER" = "virtualbox" ]]; then
	vagrant_vm=$rootdir/../$VM_DISTRO-virtualbox/
elif [[ "$VM_PROVIDER" = "libvirt" ]]; then
	vagrant_vm=$rootdir/../$VM_DISTRO-libvirt/
else
	echo "$VM_PROVIDER is not valid provider."
	echo "Please chose virtualbox or libvirt."
	exit 1
fi

trap "set +e; clean_vagrant; exit 1" SIGINT SIGTERM EXIT

function clean_vagrant {
	cd $vagrant_vm; vagrant destroy -f
	cd $rootdir; rm -rf $vagrant_vm
}

set -x
timing_enter vagrant_create_vm
timing_enter setup_vagrant_vm
cd $rootdir/..
sudo $vagrantdir/create_nvme_img.sh -s 2G
$vagrantdir/create_vbox.sh -v -s $VM_MEMORY -n $VM_CORES -p $VM_PROVIDER $VM_PROXY $VM_DISTRO
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
	vagrant ssh -c "cd /home/vagrant/spdk_repo/spdk; git clean -df"
	vagrant ssh -c "cd /home/vagrant/spdk_repo/spdk/intel-ipsec-mb; make -j$VM_CORES; sudo make install"
	vagrant ssh -c "cp /home/vagrant/spdk_repo/spdk/test/vagrant/autorun-spdk.conf /home/vagrant/"
	vagrant ssh -c "mkdir /home/vagrant/spdk_repo/output"
	vagrant ssh -c "cd /home/vagrant/spdk_repo; spdk/autorun.sh;"
	timing_exit vagrant_run_autotest
else
	timing_enter vagrant_vm_build_spdk
	vagrant ssh -c "cd /home/vagrant/spdk_repo/spdk; ./configure; make clean; make -j$VM_CORES"
	vagrant ssh -c "sudo /home/vagrant/spdk_repo/spdk/scripts/setup.sh"
	vagrant ssh -c "sudo /home/vagrant/spdk_repo/spdk/examples/nvme/hello_world/hello_world"
	timing_exit vagrant_vm_build_spdk
fi

trap - SIGINT SIGTERM EXIT
clean_vagrant
timing_exit vagrant_create_vm
