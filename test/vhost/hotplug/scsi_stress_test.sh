#!/bin/bash -xe
set -e

HOTPLUG_DIR=$(readlink -f $(dirname $0))
. $HOTPLUG_DIR/common.sh
VM_IMAGE="/home/sys_sgsw/vhost_vm_image.qcow2"
rpc_py="$SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir 1)/rpc.sock"

function run_stop_vm() {
	vms=0,$VM_IMAGE,vhost,1
	vms_setup_and_run "0"

	counter=1
	if [ $RUN_NIGHTLY -eq 1 ]; then
        	counter = 5
	fi
	for ((i=1;i<=$counter;i++)); do
        	sleep 1
	        vm_kill "0"
        	vm_run_with_arg "0"
	done

	vm_shutdown "0"
}

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR
# Stress test for hotattach/hotdetach

notice "==============="
notice ""
notice "running SPDK"
notice ""
spdk_vhost_run --vhost-num=1
$rpc_py construct_vhost_scsi_controller naa.vhost.0 --cpumask 0x2
sleep 1
for i in {0..7} ; do
	$rpc_py construct_malloc_bdev 64 512 -b Malloc$i
	$rpc_py add_vhost_scsi_lun naa.vhost.0 $i Malloc$i
done
run_stop_vm &
script_pid=$!
j=0
while kill -0 $script_pid >/dev/null 2>&1; do
	echo "Iteration: $j"
	for i in {0..7} ; do
		$rpc_py remove_vhost_scsi_target naa.vhost.0 $i
	done
	for i in {0..7} ; do
		$rpc_py add_vhost_scsi_lun naa.vhost.0 $i Malloc$i
	done
	j=$((j+1))
done

wait $script_pid
spdk_vhost_kill 1
