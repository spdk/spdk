#!/usr/bin/env bash
set -xe

HOTPLUG_DIR=$(readlink -f $(dirname $0))
. $HOTPLUG_DIR/common.sh
VM_IMAGE="/home/sys_sgsw/vhost_vm_image.qcow2"
rpc_py="$SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir 1)/rpc.sock"

function run_stop_vm() {
	trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR
	vms=0,$VM_IMAGE,vhost,1
	vms_setup_and_run "0"

	counter=3
	if [ $RUN_NIGHTLY -eq 1 ]; then
		counter=15
	fi
	for ((i=1;i<=counter;i++)); do
		vm_kill "0" "SIGKILL"
		vm_run "0"
		sleep 5
	done

	vm_kill "0" "SIGKILL"
}

function stress_test_error_exit() {
	set +e
	echo "Error on $1 - $2"
	vm_kill_all
	if [[ -n "$script_pid" ]]; then
		kill -9 $script_pid
	fi
	spdk_vhost_kill 1
	if [[ -n "$failure" ]] && [[ $failure == 1 ]]; then
		echo "Iterations $j failed"
	fi
	print_backtrace
	exit 1
}

trap 'stress_test_error_exit "${FUNCNAME}" "${LINENO}"' ERR
# Stress test for hotattach/hotdetach

notice "running SPDK"
notice "==============="
spdk_vhost_run --vhost-num=1
$rpc_py construct_vhost_scsi_controller naa.vhost.0 --cpumask [2]
for i in {0..7} ; do
	$rpc_py construct_malloc_bdev 64 512 -b Malloc$i
	$rpc_py add_vhost_scsi_lun naa.vhost.0 $i Malloc$i
done
run_stop_vm &
script_pid=$!
j=0
failure=1
while kill -0 $script_pid >/dev/null 2>&1; do
	for i in {0..7} ; do
		$rpc_py remove_vhost_scsi_target naa.vhost.0 $i
	done
	for i in {0..7} ; do
		$rpc_py add_vhost_scsi_lun naa.vhost.0 $i Malloc$i
	done
	j=$((j+1))
done
failure=0

echo "Successfully done $j iterations"
wait $script_pid
spdk_vhost_kill 1
