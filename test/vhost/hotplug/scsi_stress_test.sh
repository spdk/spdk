#!/usr/bin/env bash
set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $testdir/common.sh
rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 1)/rpc.sock"

function vm_reboot_loop() {
	trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR
	vms=0,$VM_IMAGE,vhost,1
	vms_setup_and_run "0"

	counter=1
	if [ $RUN_NIGHTLY -eq 1 ]; then
		counter=2
	fi
	for ((i=1;i<=counter;i++)); do
		vm_kill "0" "SIGKILL"
		used_vms=""
		vms_setup_and_run "0"
		sleep 5
	done

	vm_kill "0" "SIGKILL"
}

function vhost_scsi_stress_loop() {
	j=0
	failure=1
	while kill -0 $script_pid >/dev/null 2>&1; do
		for i in {0..7} ; do
			$rpc_py vhost_scsi_controller_remove_target naa.vhost.0 $i
		done
		for i in {0..7} ; do
			$rpc_py vhost_scsi_controller_add_target naa.vhost.0 $i Malloc$i
		done
		j=$((j+1))
	done
	failure=0

	echo "Successfully done $j iterations"
}

function stress_test_error_exit() {
	set +e
	echo "Error on $1 - $2"
	vm_kill_all
	if [[ -n "$script_pid" ]]; then
		kill -9 $script_pid
	fi
	vhost_kill 1
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
vhost_run 1  "-m [0,1] -p 0"
$rpc_py vhost_create_scsi_controller naa.vhost.0 --cpumask [0]
for i in {0..7} ; do
	$rpc_py bdev_malloc_create 64 512 -b Malloc$i
	$rpc_py vhost_scsi_controller_add_target naa.vhost.0 $i Malloc$i
done
vm_reboot_loop &
script_pid=$!
vhost_scsi_stress_loop
wait $script_pid
vhost_kill 1
