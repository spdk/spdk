#!/bin/bash -xe
set -e

HOTPLUG_DIR=$(readlink -f $(dirname $0))
. $HOTPLUG_DIR/common.sh
VM_IMAGE="/home/sys_sgsw/vhost_vm_image.qcow2"

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR
# Stress test for hotattach/hotdetach

notice "==============="
notice ""
notice "running SPDK"
notice ""
spdk_vhost_run
$rpc_py construct_vhost_scsi_controller naa.vhost.0 --cpumask 1
sleep 1
for i in {0..7} ; do
	$rpc_py construct_malloc_bdev 64 512 -b Malloc$i
	$rpc_py add_vhost_scsi_lun naa.vhost.0 $i Malloc$i
done
$HOTPLUG_DIR/run_stop_vm.sh --vm=0,$VM_IMAGE,vhost &
vm_pid=$!
j=0
while kill -0 $vm_pid >/dev/null 2>&1; do
	echo "Iteration: $j"
	for i in {0..7} ; do
		$rpc_py remove_vhost_scsi_target naa.vhost.0 $i
	done
	for i in {0..7} ; do
		$rpc_py add_vhost_scsi_lun naa.vhost.0 $i Malloc$i
	done
	j=$((j+1))
done

wait $vm_pid
at_app_exit
