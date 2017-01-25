#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$testdir/../../..
source $rootdir/scripts/autotest_common.sh

if [ -z "$VM_IMG" ]; then
    echo "VM_IMG: path to qcow2 image not provided - not running"
    exit 1
fi

if [ -z "$VM_QEMU" ]; then
    echo "VM_QEMU: path to qemu binary not provided - not running"
    exit 1
fi

HOST_IP=192.168.122.1
VM_IP=192.168.122.254
VM_UNAME="root"
VM_PASS="root"
VM_NAME="ext4test_vm"
VM_NET_NAME="test_net"
VM_MAC="02:de:ad:de:ad:01"
VM_BAK_IMG="/tmp/ext4test_backing.img"
TIMEO=60
SSHCMD="sshpass -p $VM_PASS ssh"
SCPCMD="sshpass -p $VM_PASS scp"

function cleanup_virsh() {
    virsh destroy $VM_NAME
    virsh net-destroy $VM_NET_NAME
    rm $VM_BAK_IMG
}

timing_enter ext4test

qemu-img create -f qcow2 -o backing_file=$VM_IMG $VM_BAK_IMG

cp $testdir/spdk_vm_base.xml $testdir/spdk_vm.xml
cp $testdir/spdk_vnet_base.xml $testdir/spdk_vnet.xml

cp $testdir/vhost.conf.in $testdir/vhost.conf
$rootdir/scripts/gen_nvme.sh >> $testdir/vhost.conf

sed -i "s@<name></name>@<name>$VM_NAME</name>@g" $testdir/spdk_vm.xml
sed -i "s@source file=''@source file='$VM_BAK_IMG'@g" $testdir/spdk_vm.xml
sed -i "s@<emulator></emulator>@<emulator>$VM_QEMU</emulator>@g" $testdir/spdk_vm.xml
sed -i "s@<name></name>@<name>$VM_NET_NAME</name>@g" $testdir/spdk_vnet.xml

trap "cleanup_virsh; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

virsh net-create $testdir/spdk_vnet.xml

# Change directory and ownership because virsh has issues with
# paths that are in /root tree
cd /tmp
$rootdir/app/vhost/vhost -c $testdir/vhost.conf &
pid=$!
echo "Process pid: $pid"
sleep 10
chmod 777 /tmp/naa.123

tar --exclude '.git' --exclude 'spdk.tgz' --exclude '*.d' --exclude '*.o' -zcf /tmp/spdk_host.tgz $rootdir

virsh create $testdir/spdk_vm.xml
virsh net-update $VM_NET_NAME add ip-dhcp-host "<host mac='$VM_MAC' name='$VM_NAME' ip='$VM_IP'/>"

# Wait for VM to boot, disable trap temporarily
# so that we don't exit on first fail
echo "Trying to connect to virtual machine..."
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
    echo "VM did not boot properly, exiting"
    exit 1
fi

$SSHCMD root@$VM_IP 'mkdir -p /tmp/spdk'
$SCPCMD -r /tmp/spdk_host.tgz root@$VM_IP:/tmp/spdk
$SSHCMD root@$VM_IP 'cd /tmp/spdk; tar xf spdk_host.tgz'
$SSHCMD root@$VM_IP '/tmp/spdk/test/vhost/ext4test/ext4connect.sh'

#read -p "Hit enter to exit..."

trap - SIGINT SIGTERM EXIT

cleanup_virsh
rm $testdir/spdk_vm.xml
rm $testdir/spdk_vnet.xml
rm $testdir/vhost.conf
killprocess $pid
timing_exit ext4test
