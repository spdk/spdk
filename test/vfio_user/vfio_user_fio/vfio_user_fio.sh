#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vfio_user/common.sh

rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"

fio_bin="--fio-bin=$FIO_BIN"
vm_no="2"

trap 'clean_vfio_user "${FUNCNAME}" "${LINENO}"' ERR
vhosttestinit

timing_enter start_vfio_user
vfio_user_run 0

#
# Create multiple malloc bdevs for multiple VMs, last VM uses nvme bdev.
#
for i in $(seq 0 $vm_no); do
	vm_muser_dir="$VM_DIR/$i/muser"
	rm -rf $vm_muser_dir
	mkdir -p $vm_muser_dir/domain/muser${i}/$i

	$rpc_py nvmf_create_subsystem nqn.2019-07.io.spdk:cnode${i} -s SPDK00${i} -a
	if ((i == vm_no)); then
		$rootdir/scripts/gen_nvme.sh | $rpc_py load_subsystem_config
		$rpc_py nvmf_subsystem_add_ns nqn.2019-07.io.spdk:cnode${i} Nvme0n1
	else
		$rpc_py bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc${i}
		$rpc_py nvmf_subsystem_add_ns nqn.2019-07.io.spdk:cnode${i} Malloc${i}
	fi
	$rpc_py nvmf_subsystem_add_listener nqn.2019-07.io.spdk:cnode${i} -t VFIOUSER -a $vm_muser_dir/domain/muser${i}/$i -s 0
done

timing_exit start_vfio_user

used_vms=""
timing_enter launch_vms
for i in $(seq 0 $vm_no); do
	vm_setup --disk-type=vfio_user --force=$i --os=$VM_IMAGE --disks="$i"
	used_vms+=" $i"
done

vm_run $used_vms
vm_wait_for_boot 60 $used_vms

timing_exit launch_vms

timing_enter run_vm_cmd

fio_disks=""
for vm_num in $used_vms; do
	qemu_mask_param="VM_${vm_num}_qemu_mask"

	host_name="VM-$vm_num-${!qemu_mask_param}"
	vm_exec $vm_num "hostname $host_name"
	vm_start_fio_server $fio_bin $vm_num
	vm_check_nvme_location $vm_num

	fio_disks+=" --vm=${vm_num}$(printf ':/dev/%s' $SCSI_DISK)"
done

job_file="default_integrity.job"
run_fio $fio_bin --job-file=$rootdir/test/vhost/common/fio_jobs/$job_file --out="$VHOST_DIR/fio_results" $fio_disks

timing_exit run_vm_cmd

vm_shutdown_all

timing_enter clean_vfio_user

for i in $(seq 0 $vm_no); do
	vm_muser_dir="$VM_DIR/$i/muser"
	$rpc_py nvmf_subsystem_remove_listener nqn.2019-07.io.spdk:cnode${i} -t vfiouser -a $vm_muser_dir/domain/muser${i}/$i -s 0
	$rpc_py nvmf_delete_subsystem nqn.2019-07.io.spdk:cnode${i}
	if ((i == vm_no)); then
		$rpc_py bdev_nvme_detach_controller Nvme0
	else
		$rpc_py bdev_malloc_delete Malloc${i}
	fi
done

vhost_kill 0
timing_exit clean_vfio_user
vhosttestfini
