#export PATH=$PATH:/usr/sbin
#sudo chmod 777 /dev/hugepages/
## Prepare env to run test
set -xe
sudo service libvirtd start

WINDOWS_SCSI_DIR=$(readlink -f $(dirname $0))
WORKSPACE=$WINDOWS_SCSI_DIR/../../../../
. $WINDOWS_SCSI_DIR/../../common/autotest_common.sh

# =================== Variables ======================= #
VM_NAME=WindowsSCSIVM
VM_MAC='00:DE:AD:DE:AD:00'
VM_IP='192.200.200.253'
VM_NET_NAME=windows_test
TIMEO=60
vhost_pid=0

function cleanup_routine() {
    sudo virsh destroy $VM_NAME || true
    sleep 5
    sudo virsh net-destroy $VM_NET_NAME || true
    sudo kill $(cat $WORKSPACE/vhost.pid) || true
    sudo chmod g-w /dev/hugepages
    sudo service libvirtd stop
}

trap "cleanup_routine; exit 1" SIGINT SIGTERM EXIT

# =================== SET UP WORKSPACE ======================= #

mkdir -p $WORKSPACE/windows
mkdir -p $WORKSPACE/results

# Copy and prepare config files
cp $WINDOWS_SCSI_DIR/vhost_base.conf $WORKSPACE/vhost.conf
truncate -s 256M $WORKSPACE/aio.disk


cp $WINDOWS_SCSI_DIR/windows_vm.xml $WORKSPACE/windows/windows_vm.xml
cp $WINDOWS_SCSI_DIR/windows_vnet.xml $WORKSPACE/windows/windows_vnet.xml

sed -i "s#<name></name>#<name>$VM_NAME</name>#g" $WORKSPACE/windows/windows_vm.xml
sed -i "s#<source file=/>#<source file='$WORKSPACE/windows/windows_test.img'/>#g" $WORKSPACE/windows/windows_vm.xml
sed -i "s#path=SOCKET_PATH_SUB#path=$WORKSPACE/spdk/vhost.0#g" $WORKSPACE/windows/windows_vm.xml
sed -i "s#<name></name>#<name>$VM_NET_NAME</name>#g" $WORKSPACE/windows/windows_vnet.xml

# Create backing image of OS image
sudo qemu-img create -f qcow2 -o backing_file=/home/sys_sgsw/windows_scsi_compliance/windows_vm_image.qcow2 $WORKSPACE/windows/windows_test.img

# Start virtual network
sudo virsh net-create $WORKSPACE/windows/windows_vnet.xml; sleep 1
sudo virsh net-update $VM_NET_NAME add ip-dhcp-host "<host mac='$VM_MAC' name='$VM_NAME' ip='$VM_IP'/>"

# =================== START STUFF ======================= #
# start vhost
#cd $WORKSPACE/spdk
#git submodule update --init
#./configure
#make clean
#make -j100
#$WINDOWS_SCSI_DIR/../../../scripts/setup.sh reset
#NRHUGE=15000 WINDOWS_SCSI_DIR/../../../scripts/setup.sh
$WINDOWS_SCSI_DIR/../../../spdk/scripts/gen_nvme.sh >> $WORKSPACE/vhost.conf

#Below - temporary. Setup.sh needs to add write permissions for group so that libvirt can acces HP's
sudo chmod g+w /dev/hugepages
sudo $WINDOWS_SCSI_DIR/../../../app/vhost/vhost -s 2048 -c $WORKSPACE/vhost.conf -f $WORKSPACE/vhost.pid &
vhost_pid=$!
echo "Vhost Process pid: $vhost_pid"
sleep 30

sudo chmod 777 $WORKSPACE/spdk/vhost.0

# start Windows VM
sudo virsh create $WORKSPACE/windows/windows_vm.xml

set +xe

# Wait until VM goes up
TIMEO=240
rc=-1
while [[ $TIMEO -gt 0 && rc -ne 0 ]]; do
    sshpass -p 'R00tr00t$$' ssh -o ConnectTimeout=1 root@192.200.200.253 "true"
    rc=$?
    ((TIMEO-=1))
    sleep 1
done
if [[ $TIMEO -eq 0  ||  rc -ne 0 ]]; then
    echo "ERROR: VM did not boot properly, exiting"
    exit 1
fi
echo "INFO: VM booted; Waiting a while and starting tests"
sleep 30

sshpass -p 'R00tr00t$$' ssh root@192.200.200.253 "cd /cygdrive/c/SCSI; powershell.exe -file compliance_test.ps1"
sshpass -p 'R00tr00t$$' scp root@192.200.200.253:/cygdrive/c/SCSI/WIN_SCSI_* $WORKSPACE/results/
sudo virsh destroy $VM_NAME
dos2unix $WORKSPACE/results/WIN_SCSI_*.log

trap - SIGINT SIGTERM EXIT
cleanup_routine
