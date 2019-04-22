#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

#Checking proxy if you need.
if [ -z $http_proxy ] || [ -z $https_proxy ] || [ -z $SPDK_VAGRANT_HTTP_PROXY ] ; then
echo "Here you should set the right proxy for downloading!"
echo "Please export right proxy http_proxy= ;https_proxy= ;SPDK_VAGRANT_HTTP_PROXY= "
echo "If you don't need to have a proxy setting ,please overlook this, and continue!"
fi

# The default value is decided by the real cases in VM, confirm the value is good for auto running.
VM_OS=${VM_OS:-fedora28}
MEMORY_SIZE=${MEMORY_SIZE:-16384}
NCPU=${NCPU:-16}
PROVIDER=${PROVIDER:-virtualbox}

echo "Check configuration..."
echo "VM_OS = $VM_OS"
echo "MEMORY_SIZE = $MEMORY_SIZE"
echo "NCPU = $NCPU"

#This is compatible with libvirt,if switch between virtualbox and libvirt, need to kill some processes,and well installed packages.

PROVIDER=${SPDK_VAGRANT_PROVIDER:-virtualbox}

if [ -n "$PROVIDER" ]; then
	provider="--provider=${PROVIDER}"
fi

# Function for vagrant operations
function vagrant_get_id()
{
	vagrant global-status | grep $VAGRANT_DIR | awk '{print $1}'
}

function vagrant_get_powerstatus()
{
	vagrant global-status | grep $VAGRANT_DIR | awk '{print $4}'
}

function vagrant_up()
{
	vagrant up $(vagrant_get_id) --provision
}

function vagrant_reload()
{
	vagrant reload $(vagrant_get_id) --provision
}

function vagrant_provision()
{
	if [[ "$(vagrant_get_powerstatus)" = poweroff && $PROVIDER = virtualbox ]]; then
		vagrant plugin install vagrant-vbguest
		vagrant_up || true
	elif [ "$(vagrant_get_powerstatus)" = shutoff ]; then
		vboxmanage hostonlyif remove vboxnet0 || true
		vagrant_up || true
	elif [ $PROVIDER = libvirt ]; then
		vagrant_up || true
	fi
	vagrant provision $(vagrant_get_id)
}

function vagrant_halt()
{
	if [ "$(vagrant_get_powerstatus)" = running ]; then
		vagrant halt $(vagrant_get_id)
	fi
}

function vagrant_run_cmd()
{
	vagrant ssh $(vagrant_get_id) -c "$@"
}

function ssh_login_machine()
{
	vagrant ssh $(vagrant_get_id)
}

function usage()
{
	cat <<EOF
Usage: $0 [-resvch] [--recreate|eradicate|ssh-machine|vhost|check|help]
	Command usage:
				If need to switch HW config:
				   export NCPU=2
				   export VM_OS=fedora27/fedora26/ubuntu18/ubuntu16
				   export MEMORY_SIZE=4096
				If need proxy, please config these:
				   export http_proxy=
				   export https_proxy=
				   export SPDK_VAGRANT_HTTP_PROXY=
				If provider is libvirt, need to export:
				   if switch provider,use htop to watch occupied resources,and kill them.
				   export SPDK_VAGRANT_PROVIDER=libvirt
				If need run cases need to edit file:
				    autorun-spdk.conf
	-r|--recreate :	Recreate the virtual machine regardless of its existence.
	-e|--eradicate:	After running the case, eradicate the virtual machine.
	              : Without parameters means a VM which already exists. If there is no VM, it will
	                create a new default VM with VM_OS=fedora28, MEMORY_SIZE=16384, NCPU=16.
	                if you want to change them ,please export variables. Such as export NCPU=8,
	                MEMORY_SIZE=8192,VM_OS=fedora26.
	-s|--ssh-machine: ssh login VM. This is designed for developer.we can revise debug code here.
	-v|--vhost: vhost related create, default create ubuntu18,also can export VM_VHOST_OS=ubuntu16.
	-c|--check: help to list all the VMs. and HDDs , NVME that attached to VM.
				If need to display virtual brige
				   vboxmanage list bridgedifs
				If need to unregister VM
				   vboxmanage list vms
				   vboxmanage unregistervm UUID --delete || true
				If need to remove HDD or NVMe
				   vboxmanage list hdds
				   vboxmanage closemedium  UUID --delete || true
	-h|--help    : Show help.
EOF
}

function check_vm_status()
{
	vboxmanage list vms
	vboxmanage list hdds
	vagrant global-status --prune
    echo "If report HDD/Nvme is in using ,"vboxmanage closemedium  UUID --delete" will report the wrong UUID"
    echo "please kill the processes that are still running."
}

function vm_create()
{
	cd $rootdir/..
	if [ ! -d $VM_OS-$PROVIDER ]; then
		spdk/scripts/vagrant/create_vbox.sh -p $PROVIDER -s $MEMORY_SIZE -n $NCPU $VM_OS
	fi
	cd $rootdir
}

function vm_destroy()
{
	vagrant_halt
	vboxmanage unregistervm $VM_UUID --delete || true
	rm -rf $rootdir/../$VM_OS-$PROVIDER
}

function vm_setup_env()
{
	vagrant_run_cmd "sudo spdk_repo/spdk/scripts/vagrant/update.sh"
	vagrant_run_cmd "sudo spdk_repo/spdk/test/common/config/vm_setup.sh -i"
}

function vm_check_nvme_device()
{
	local cmdset="cd spdk_repo/spdk;\
		git submodule update --init;\
		./configure --enable-debug && make;\
		sudo scripts/setup.sh;\
		cd examples/nvme/hello_world;\
		sudo ./hello_world"
	vagrant_run_cmd "$cmdset"
}

function vm_run_autotest()
{
	vagrant_run_cmd "sudo rm -rf /home/vagrant/vagrant"
	vagrant_run_cmd "sudo cp /home/vagrant/spdk_repo/spdk/scripts/vagrant/autorun-spdk.conf /home/vagrant/autorun-spdk.conf"
	vagrant_run_cmd "sudo cp /home/vagrant/autorun-spdk.conf /root/autorun-spdk.conf"
	vagrant_run_cmd "sudo rm -rf /home/vagrant/latest && sudo rm -rf /home/vagrant/master"
	vagrant_run_cmd "sudo ~/spdk_repo/spdk/scripts/vagrant/run-autorun.sh -d /home/vagrant/spdk_repo/spdk"
}

function VM_create_demo_check()
{
	vm_create $MEMORY_SIZE $NCPU $VM_OS
	VM_UUID=$(find $VAGRANT_DIR/.vagrant -type f -name id -exec cat {} +)
	vm_setup_env
	vm_check_nvme_device
}

function unset_variables()
{
	unset VM_OS
	unset MEMORY_SIZE
	unset NCPU
	unset PROVIDER
	unset SPDK_VAGRANT_PROVIDER
}

function vhost_create_vm()
{
	#This is left for further vhost completion.
	echo "Start vhost setup..."
	cd $rootdir
	VM_VHOST_OS=${VM_VHOST_OS:-ubuntu18}
	if [[ ! -d "$rootdir/$VM_VHOST_OS" && ! -d "$rootdir/scripts/vagrant/$VM_VHOST_OS" ]]; then
		$rootdir/scripts/vagrant/create_vhost_vm.sh  $VM_VHOST_OS
	else
		echo " Already existed vhost folder $VM_VHOST_OS. "
	fi
	unset VM_VHOST_OS
}

function vm_setup_and_autotest()
{
	cd $rootdir
	VAGRANT_DIR="$(readlink -f ../$VM_OS-$PROVIDER)"
	if [ $VM_VHOST = true ]; then
		vhost_create_vm
		exit 0
	fi
	trap "vm_destroy $VM_OS-$PROVIDER;exit" SIGINT SIGTERM ERR
	if [[ $VM_RECREATE = true || ! -d "$VAGRANT_DIR" ]]; then
		if [ -d "$VAGRANT_DIR" ]; then
			VM_UUID=$(find $VAGRANT_DIR/.vagrant -type f -name id -exec cat {} +)
			vm_destroy $VM_OS-$PROVIDER
		fi
		VM_create_demo_check
	else
		trap - SIGINT SIGTERM ERR
		VM_UUID=$(find $VAGRANT_DIR/.vagrant -type f -name id -exec cat {} +)
		vagrant_provision
	fi
	trap - SIGINT SIGTERM ERR
	if [ $VM_LOGIN = true ]; then
		echo "Start to login a VM that finished with demo check."
		ssh_login_machine
		exit 0
	fi
	echo "Start testing autotest cases in the vm of $VM_OS"
	vm_run_autotest
	echo "Start to destroy $VM_OS"
	if [ $VM_ERADICATE = true ]; then
		vm_destroy $VM_OS-$PROVIDER
		unset_variables
	fi
}

set -- $(getopt -o resvch --longoptions recreate,eradicate,ssh-machine,vhost,check,help -- "$@")
while true; do
	case "$1" in
	-r|--recreate)
		VM_RECREATE=true
		shift 1
	;;
	-e|--eradicate)
		VM_ERADICATE=true
		shift 1
	;;
	-s|--ssh-machine)
		VM_LOGIN=true
		shift 1
	;;
	-v|--vhost)
		VM_VHOST=true
		shift 1
	;;
	-c|--check)
		check_vm_status
		exit 0
	;;
	-h|--help)
		usage
		exit 0
	;;
	--) break
	;;
	*)  echo "Invalid argument!"
		usage
		exit 1
	;;
	esac
done


: ${VM_RECREATE:=false}
: ${VM_ERADICATE:=false}
: ${VM_LOGIN:=false}
: ${VM_VHOST:=false}

case "$VM_OS" in
	fedora28|fedora27|fedora26|ubuntu18|ubuntu16)
	   vm_setup_and_autotest
	;;
	freebsd11|centos7)
	   echo "Option unavailable."
	   exit 1
	;;
	*)
	   echo " no such OS supported! "
	   exit 1
	;;
esac
