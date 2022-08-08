#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vfio_user/common.sh

bdfs=($(get_nvme_bdfs))
rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"

trap 'clean_vfio_user "${FUNCNAME}" "${LINENO}"' ERR EXIT

vhosttestinit

vfio_user_run 0

vm_muser_dir="$VM_DIR/1/muser"
rm -rf $vm_muser_dir
mkdir -p $vm_muser_dir/domain/muser1/1

$rpc_py bdev_nvme_attach_controller -b Nvme0 -t pcie -a ${bdfs[0]}
$rpc_py nvmf_create_subsystem nqn.2019-07.io.spdk:cnode1 -s SPDK001 -a
$rpc_py nvmf_subsystem_add_ns nqn.2019-07.io.spdk:cnode1 Nvme0n1
$rpc_py nvmf_subsystem_add_listener nqn.2019-07.io.spdk:cnode1 -t VFIOUSER -a $vm_muser_dir/domain/muser1/1 -s 0

vm_setup --disk-type=vfio_user --force=1 --os=$VM_IMAGE --disks="1"
vm_run 1
vm_wait_for_boot 60 1

vm_exec 1 "lsblk"
# execute "poweroff" for vm 1
vm_shutdown_all

# re-launch the vm to see if memory region register / unregister will failed
vm_setup --disk-type=vfio_user --force=1 --os=$VM_IMAGE --disks="1"
vm_run 1
vm_wait_for_boot 60 1

vm_exec 1 "lsblk"

vm_shutdown_all

$rpc_py nvmf_subsystem_remove_listener nqn.2019-07.io.spdk:cnode1 -t vfiouser -a $vm_muser_dir/domain/muser1/1 -s 0
$rpc_py nvmf_delete_subsystem nqn.2019-07.io.spdk:cnode1
$rpc_py bdev_nvme_detach_controller Nvme0

vhost_kill 0

trap - ERR EXIT

vhosttestfini
