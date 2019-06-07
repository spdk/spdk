#!/usr/bin/env bash
set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

os_image="/home/sys_sgci/openstack_vm_image.qcow2"
vm_no=0

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR

vm_setup --force=$vm_no --os=$os_image --queue_num=8 --memory=8192\
 --qemu-args='-drive format=raw,file=/home/sys_sgsw/openstack_nvme.img,if=none,id=openstack_nvme -device nvme,drive=openstack_nvme,serial=deadbeef'
vm_run $vm_no
vm_wait_for_boot 300 $vm_no
vm_ssh $vm_no "sudo su -- stack; cd  /opt/stack/devstack; ./unstack.sh; ./stack.sh"
vm_ssh $vm_no "cd /home/vagrant; git clone https://review.gerrithub.io/spdk/spdk"
vm_ssh $vm_no "sudo NRHUGE=1024 /home/vagrant/spdk/scripts/setup.sh"
vm_ssh $vm_no "cd /home/vagrant//spdk; make clean; ./configure --with-rdma; make -j10"
vm_ssh $vm_no "sudo /home/vagrant/spdk/test/openstack/run_openstack_tests.sh"

vm_shutdown_all
