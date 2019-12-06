#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh
source $rootdir/test/vhost/hotplug/common.sh

function prepare_fio_cmd_tc1() {
	print_test_fio_header

	run_fio="$fio_bin --eta=never "
	for vm_num in $1; do
		cp $fio_job $tmp_attach_job
		vm_check_scsi_location $vm_num
		for disk in $SCSI_DISK; do
			echo "[nvme-host$disk]" >> $tmp_attach_job
			echo "filename=/dev/$disk" >> $tmp_attach_job
		done
		vm_scp $vm_num $tmp_attach_job 127.0.0.1:/root/default_integrity_discs.job
		run_fio+="--client=127.0.0.1,$(vm_fio_socket ${vm_num}) --remote-config /root/default_integrity_discs.job "
		rm $tmp_attach_job
	done
}

# Check if fio test passes on device attached to first controller.
function hotattach_tc1() {
	notice "Hotattach test case 1"

	$rpc_py vhost_scsi_controller_add_target naa.Nvme0n1p0.0 0 Nvme0n1p0

	sleep 3
	prepare_fio_cmd_tc1 "0"
	$run_fio
	check_fio_retcode "Hotattach test case 1: Iteration 1." 0 $?
}

# Run fio test for previously attached device.
# During test attach another device to first controller and check fio status.
function hotattach_tc2() {
	notice "Hotattach test case 2"
	prepare_fio_cmd_tc1 "0"

	$run_fio &
	last_pid=$!
	sleep 3
	$rpc_py vhost_scsi_controller_add_target naa.Nvme0n1p0.0 1 Nvme0n1p1
	wait $last_pid
	check_fio_retcode "Hotattach test case 2: Iteration 1." 0 $?
}

# Run fio test for previously attached devices.
# During test attach another device to second controller and check fio status.
function hotattach_tc3() {
	notice "Hotattach test case 3"
	prepare_fio_cmd_tc1 "0"

	$run_fio &
	last_pid=$!
	sleep 3
	$rpc_py vhost_scsi_controller_add_target naa.Nvme0n1p1.0 0 Nvme0n1p2
	wait $last_pid
	check_fio_retcode "Hotattach test case 3: Iteration 1." 0 $?
}

# Run fio test for previously attached devices.
# During test attach another device to third controller(VM2) and check fio status.
# At the end after rebooting VMs run fio test for all devices and check fio status.
function hotattach_tc4() {
	notice "Hotattach test case 4"

	prepare_fio_cmd_tc1 "0"

	$run_fio &
	last_pid=$!
	sleep 3
	$rpc_py vhost_scsi_controller_add_target naa.Nvme0n1p2.1 0 Nvme0n1p3
	wait $last_pid
	check_fio_retcode "Hotattach test case 4: Iteration 1." 0 $?

	prepare_fio_cmd_tc1 "0 1"
	$run_fio
	check_fio_retcode "Hotattach test case 4: Iteration 2." 0 $?

	reboot_all_and_prepare "0 1"

	prepare_fio_cmd_tc1 "0 1"
	$run_fio
	check_fio_retcode "Hotattach test case 4: Iteration 3." 0 $?
}

function cleanup_after_tests() {
	$rpc_py vhost_scsi_controller_remove_target naa.Nvme0n1p0.0 0
	$rpc_py vhost_scsi_controller_remove_target naa.Nvme0n1p0.0 1
	$rpc_py vhost_scsi_controller_remove_target naa.Nvme0n1p1.0 0
	$rpc_py vhost_scsi_controller_remove_target naa.Nvme0n1p2.1 0
}

hotattach_tc1
hotattach_tc2
hotattach_tc3
hotattach_tc4
cleanup_after_tests
