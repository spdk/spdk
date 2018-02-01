#!/usr/bin/env bash

set -xe

: ${QEMU_PREFIX="/usr/local/qemu/spdk-2.12-pre"}

basedir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $basedir/../../..)
testdir=$(readlink -f $rootdir/..)
MAKE="make -j$(( $(nproc)  * 2 ))"

rpc_py="python $rootdir/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"
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

while getopts 'i:m:f:' optchar; do
    case $optchar in
        i) VM_IMG="${OPTARG#*=}" ;;
        m) VHOST_MODE="${OPTARG#*=}" ;;
        f) VM_FS="${OPTARG#*=}" ;;
    esac
done

source $rootdir/scripts/autotest_common.sh

if [ -z "$VM_IMG" ]; then
    echo "ERROR: VM_IMG: path to qcow2 image not provided - not running"
    exit 1
fi

if [ -z "$VHOST_MODE" ]; then
    echo "ERROR: VHOST_MODE: please specify Vhost mode - scsi or blk"
fi

if [ -z "$VM_FS" ]; then
    VM_FS="ext4"
    echo "INFO: Using default value for filesystem: $VM_FS"
fi

# Check if Qemu binary is present
if [[ -z $VM_QEMU ]]; then
    VM_QEMU="$QEMU_PREFIX/bin/qemu-system-x86_64"
fi

if [[ ! -x $VM_QEMU ]]; then
    echo "ERROR: QEMU binary not present in $VM_QEMU"
fi

if [[ -z $QEMU_IMG ]]; then
    QEMU_IMG="$QEMU_PREFIX/bin/qemu-img"
fi

echo "Running test with filesystem: $VM_FS"

function cleanup_virsh() {
    if virsh domstate $VM_NAME; then
        virsh shutdown $VM_NAME
        for timeo in `seq 0 10`; do
            if ! virsh domstate $VM_NAME; then
                break
            fi
            if [[ $timeo -eq 10 ]]; then
                echo "ERROR: VM did not shutdown, killing!"
                virsh destroy $VM_NAME
            fi
            sleep 1
        done
    fi

    if virsh net-info $VM_NET_NAME; then
        virsh net-destroy $VM_NET_NAME
    fi
    rm $VM_BAK_IMG || true
}

function cleanup_lvol() {
    echo "INFO: Removing lvol bdevs"
    $rpc_py delete_bdev $lb_name
    echo -e "\tINFO: lvol bdev $lb_name removed"

    echo "INFO: Removing lvol stores"
    $rpc_py destroy_lvol_store -u $lvol_store
    echo -e "\tINFO: lvol stote $lvol_store removed"
}

timing_enter integrity_test

# Backing image for VM
"$QEMU_IMG" create -f qcow2 -o backing_file=$VM_IMG $VM_BAK_IMG

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
if [[ "$VHOST_MODE" == "scsi" ]]; then
    sed -i "s@vhost_dev_args@vhost-user-scsi-pci,id=scsi0@g" $basedir/vm_conf.xml
else
    sed -i "s@vhost_dev_args@vhost-user-blk-pci@g" $basedir/vm_conf.xml
fi

trap "cleanup_virsh; killprocess $pid; exit 1" SIGINT SIGTERM EXIT

virsh net-create $basedir/vnet_conf.xml

# Change directory and ownership because virsh has issues with
# paths that are in /root tree
cd /tmp
$rootdir/app/vhost/vhost -c $basedir/vhost.conf &
pid=$!
echo "Process pid: $pid"
waitforlisten "$pid"

lvol_store=$($rpc_py construct_lvol_store Nvme0n1 lvs_0)
free_mb=$(get_lvs_free_mb "$lvol_store")
lb_name=$($rpc_py construct_lvol_bdev -u $lvol_store lbd_0 $free_mb)

if [[ "$VHOST_MODE" == "scsi" ]]; then
    $rpc_py construct_vhost_scsi_controller naa.0
    $rpc_py add_vhost_scsi_lun naa.0 0 $lb_name
else
    $rpc_py construct_vhost_blk_controller naa.0 $lb_name
fi

trap "cleanup_lvol; cleanup_virsh; killprocess $pid; exit 1" SIGINT SIGTERM EXIT ERR

chmod 777 /tmp/naa.0

virsh create $basedir/vm_conf.xml
virsh net-update $VM_NET_NAME add ip-dhcp-host "<host mac='$VM_MAC' name='$VM_NAME' ip='$VM_IP'/>"

# Wait for VM to boot
echo "INFO: Trying to connect to virtual machine..."
while ! $SSHCMD root@$VM_IP -q -oStrictHostKeyChecking=no 'echo Hello'; do
    sleep 1
    if ! (( TIMEO-=1 ));then
        echo "ERROR: VM did not boot properly, exiting"
        exit 1
    fi
done

# Run test on Virtual Machine
$SCPCMD -r $basedir/integrity_vm.sh root@$VM_IP:~
$SSHCMD root@$VM_IP "fs=$VM_FS ~/integrity_vm.sh $VHOST_MODE"

# Kill VM, cleanup config files
cleanup_virsh
rm $basedir/vm_conf.xml || true
rm $basedir/vnet_conf.xml || true
rm $basedir/vhost.conf || true

# Delete lvol bdev, destroy lvol store
cleanup_lvol

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
