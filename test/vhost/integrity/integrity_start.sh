#!/usr/bin/env bash

basedir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $basedir/../../..)
testdir=$(readlink -f $rootdir/..)
qemu_src_dir="$testdir/qemu"
qemu_build_dir="$testdir/qemu/build"
qemu_install_dir="$testdir/root"
MAKE="make -j$(( $(nproc)  * 2 ))"

source $rootdir/scripts/autotest_common.sh

if [ -z "$VM_IMG" ]; then
    echo "ERROR: VM_IMG: path to qcow2 image not provided - not running"
    exit 1
fi

if [ -z "$VM_QEMU" ]; then
    echo "INFO: VM_QEMU: path to qemu binary not provided"
    echo "INFO: Will use qemu from repository"
fi

if [ -z "$VM_FS" ]; then
    VM_FS="ext4"
    echo "INFO: Using default value for filesystem: $VM_FS"
fi

HOST_IP=192.200.200.1
VM_IP=192.200.200.254
VM_UNAME="root"
VM_PASS="root"
VM_NAME="int_test_vm"
VM_NET_NAME="int_test_net"
VM_MAC="02:de:ad:de:ad:01"
VM_BAK_IMG="/tmp/int_test_backing.img"
TIMEO=60
SSHCMD="sshpass -p $VM_PASS ssh"
SCPCMD="sshpass -p $VM_PASS scp"

echo "FS: $VM_FS"

function cleanup_virsh() {
    virsh shutdown $VM_NAME || true
    sleep 5
    virsh net-destroy $VM_NET_NAME || true
    rm $VM_BAK_IMG || true
}

timing_enter integrity_test

# If no VM_QEMU argument is given - check if needed qemu is installed
echo "INFO: Checking qemu..."
if [[ ! -d $qemu_src_dir && -z "$VM_QEMU" ]]; then
    echo "INFO: Cloning $qemu_src_dir"
    rm -rf $qemu_src_dir
    mkdir -p $qemu_src_dir
    cd $(dirname $qemu_src_dir)
    git clone -b dev/vhost_scsi ssh://az-sg-sw01.ch.intel.com:29418/qemu
    echo "INFO: Cloning Qemu Done"
else
    echo "INFO: Qemu source exist $qemu_src_dir - not cloning"
fi

# Check if Qemu binary is present; build it if not
if [[ ! -x $qemu_install_dir/bin/qemu-system-x86_64 && -z "$VM_QEMU" ]]; then
    echo "INFO: Can't find $qemu_install_dir/bin/qemu-system-x86_64 - building and installing"
    mkdir -p $qemu_build_dir
    cd $qemu_build_dir

    $qemu_src_dir/configure --prefix=$qemu_install_dir \
    --target-list="x86_64-softmmu" \
    --enable-kvm --enable-linux-aio --enable-numa

    echo "INFO: Compiling and installing QEMU in $qemu_install_dir"
    $MAKE install
    VM_QEMU="$qemu_install_dir/bin/qemu-system-x86_64"
    echo "INFO: DONE"
elif [[ -z "$VM_QEMU" ]]; then
    VM_QEMU="$qemu_install_dir/bin/qemu-system-x86_64"
fi

# Backing image for VM
qemu-img create -f qcow2 -o backing_file=$VM_IMG $VM_BAK_IMG

# Prepare vhost config
cp $basedir/vhost.conf.in $basedir/vhost.conf
$rootdir/scripts/gen_nvme.sh >> $basedir/vhost.conf

# Prepare .xml files for Virsh
cp $basedir/base_vm.xml $basedir/vm_conf.xml
cp $basedir/base_vnet.xml $basedir/vnet_conf.xml
sed -i "s@<name></name>@<name>$VM_NAME</name>@g" $basedir/vm_conf.xml
sed -i "s@source file=''@source file='$VM_BAK_IMG'@g" $basedir/vm_conf.xml
sed -i "s@<emulator></emulator>@<emulator>$VM_QEMU</emulator>@g" $basedir/vm_conf.xml
sed -i "s@mac address=''@mac address='$VM_MAC'@g" $basedir/vm_conf.xml
sed -i "s@source network=''@source network='$VM_NET_NAME'@g" $basedir/vm_conf.xml
sed -i "s@<name></name>@<name>$VM_NET_NAME</name>@g" $basedir/vnet_conf.xml

trap "cleanup_virsh; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

virsh net-create $basedir/vnet_conf.xml

# Change directory and ownership because virsh has issues with
# paths that are in /root tree
cd /tmp
$rootdir/app/vhost/vhost -c $basedir/vhost.conf &
pid=$!
echo "Process pid: $pid"
sleep 10
chmod 777 /tmp/naa.0

virsh create $basedir/vm_conf.xml
virsh net-update $VM_NET_NAME add ip-dhcp-host "<host mac='$VM_MAC' name='$VM_NAME' ip='$VM_IP'/>"

# Wait for VM to boot, disable trap temporarily
# so that we don't exit on first fail
echo "INFO: Trying to connect to virtual machine..."
trap - SIGINT SIGTERM EXIT
set +xe
rc=-1
while [[ $TIMEO -gt 0 && rc -ne 0 ]]; do
    $SSHCMD root@$VM_IP -q -oStrictHostKeyChecking=no 'echo Hello'
    rc=$?
    ((TIMEO-=1))
done
set -xe
trap "cleanup_virsh; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

if [[ $TIMEO -eq 0  ||  rc -ne 0 ]]; then
    echo "ERROR: VM did not boot properly, exiting"
    exit 1
fi

# Run test on Virtual Machine
$SCPCMD -r $basedir/integrity_vm.sh root@$VM_IP:~
$SSHCMD root@$VM_IP "fs=$VM_FS ~/integrity_vm.sh"

# Kill VM, cleanup config files
cleanup_virsh
rm $basedir/vm_conf.xml || true
rm $basedir/vnet_conf.xml || true
rm $basedir/vhost.conf || true

# Try to gracefully stop spdk vhost
if /bin/kill -INT $pid; then
    while /bin/kill -0 $pid; do
        sleep 1
    done
elif /bin/kill -0 $pid; then
    killprocess $pid
    echo "ERROR: Vhost was not closed gracefully..."
    exit 1
else
    exit 1
fi

trap - SIGINT SIGTERM EXIT
timing_exit integrity_test
