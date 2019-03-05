#!/usr/bin/env bash
set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $testdir/common.sh
rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 1)/rpc.sock"

function run_stop_vm() {
	trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR
	vms=0,$VM_IMAGE,vhost,1
	vms_setup_and_run "0"

	counter=2
	if [ $RUN_NIGHTLY -eq 1 ]; then
		counter=6
	fi
	for ((i=1;i<=counter;i++)); do
		vm_kill "0" "SIGKILL"
		used_vms=""
		vms_setup_and_run "0"
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
vhost_run 1 "-m 0x6"
$rpc_py construct_vhost_scsi_controller naa.vhost.0 --cpumask [2]
for i in {0..7} ; do
	$rpc_py bdev_malloc_create 64 512 -b Malloc$i
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
vhost_kill 1
