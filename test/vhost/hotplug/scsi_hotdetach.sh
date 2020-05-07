#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh
source $rootdir/test/vhost/hotplug/common.sh

function get_first_disk() {
	vm_check_scsi_location $1
	disk_array=($SCSI_DISK)
	eval "$2=${disk_array[0]}"
}

function check_disks() {
	if [ "$1" == "$2" ]; then
		fail "Disk has not been deleted"
	fi
}

function prepare_fio_cmd_tc1_iter1() {
	print_test_fio_header

	run_fio="$fio_bin --eta=never "
	for vm_num in $1; do
		cp $fio_job $tmp_detach_job
		vm_check_scsi_location $vm_num
		for disk in $SCSI_DISK; do
			echo "[nvme-host$disk]" >> $tmp_detach_job
			echo "filename=/dev/$disk" >> $tmp_detach_job
		done
		vm_scp "$vm_num" $tmp_detach_job 127.0.0.1:/root/default_integrity_4discs.job
		run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity_4discs.job "
		rm $tmp_detach_job
	done
}

function prepare_fio_cmd_tc2_iter1() {
	print_test_fio_header

	run_fio="$fio_bin --eta=never "
	for vm_num in $1; do
		cp $fio_job $tmp_detach_job
		vm_check_scsi_location $vm_num
		disk_array=($SCSI_DISK)
		disk=${disk_array[0]}
		echo "[nvme-host$disk]" >> $tmp_detach_job
		echo "filename=/dev/$disk" >> $tmp_detach_job
		vm_scp "$vm_num" $tmp_detach_job 127.0.0.1:/root/default_integrity.job
		run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity.job "
		rm $tmp_detach_job
	done
}

function prepare_fio_cmd_tc2_iter2() {
	print_test_fio_header

	run_fio="$fio_bin --eta=never "
	for vm_num in $1; do
		cp $fio_job $tmp_detach_job
		if [ $vm_num == 2 ]; then
			vm_job_name=default_integrity_3discs.job
		else
			vm_job_name=default_integrity_4discs.job
		fi
		vm_check_scsi_location $vm_num
		for disk in $SCSI_DISK; do
			echo "[nvme-host$disk]" >> $tmp_detach_job
			echo "filename=/dev/$disk" >> $tmp_detach_job
		done
		vm_scp "$vm_num" $tmp_detach_job 127.0.0.1:/root/$vm_job_name
		run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/${vm_job_name} "
		rm $tmp_detach_job
	done
}

function prepare_fio_cmd_tc3_iter1() {
	print_test_fio_header

	run_fio="$fio_bin --eta=never "
	for vm_num in $1; do
		cp $fio_job $tmp_detach_job
		if [ $vm_num == 2 ]; then
			vm_job_name=default_integrity_3discs.job
		else
			vm_job_name=default_integrity_4discs.job
		fi
		vm_check_scsi_location $vm_num
		j=1
		for disk in $SCSI_DISK; do
			if [ $vm_num == 2 ]; then
				if [ $j == 1 ]; then
					((j++))
					continue
				fi
			fi
			echo "[nvme-host$disk]" >> $tmp_detach_job
			echo "filename=/dev/$disk" >> $tmp_detach_job
			((j++))
		done
		vm_scp "$vm_num" $tmp_detach_job 127.0.0.1:/root/$vm_job_name
		run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/$vm_job_name "
		rm $tmp_detach_job
	done
}

# During fio test for all devices remove first device from fifth controller and check if fio fails.
# Also check if disc has been removed from VM.
function hotdetach_tc1() {
	notice "Hotdetach test case 1"
	first_disk=""
	get_first_disk "2" first_disk
	prepare_fio_cmd_tc1_iter1 "2 3"
	$run_fio &
	last_pid=$!
	sleep 3
	$rpc_py vhost_scsi_controller_remove_target naa.Nvme0n1p4.2 0
	set +xe
	wait $last_pid
	check_fio_retcode "Hotdetach test case 1: Iteration 1." 1 $?
	set -xe
	second_disk=""
	get_first_disk "2" second_disk
	check_disks $first_disk $second_disk
	clear_after_tests
}

# During fio test for device from third VM remove first device from fifth controller and check if fio fails.
# Also check if disc has been removed from VM.
function hotdetach_tc2() {
	notice "Hotdetach test case 2"
	sleep 2
	first_disk=""
	get_first_disk "2" first_disk
	prepare_fio_cmd_tc2_iter1 "2"
	$run_fio &
	last_pid=$!
	sleep 3
	$rpc_py vhost_scsi_controller_remove_target naa.Nvme0n1p4.2 0
	set +xe
	wait $last_pid
	check_fio_retcode "Hotdetach test case 2: Iteration 1." 1 $?
	set -xe
	second_disk=""
	get_first_disk "2" second_disk
	check_disks $first_disk $second_disk
	clear_after_tests
}

# Run fio test for all devices except one, then remove this device and check if fio passes.
# Also check if disc has been removed from VM.
function hotdetach_tc3() {
	notice "Hotdetach test case 3"
	sleep 2
	first_disk=""
	get_first_disk "2" first_disk
	prepare_fio_cmd_tc3_iter1 "2 3"
	$run_fio &
	last_pid=$!
	sleep 3
	$rpc_py vhost_scsi_controller_remove_target naa.Nvme0n1p4.2 0
	wait $last_pid
	check_fio_retcode "Hotdetach test case 3: Iteration 1." 0 $?
	second_disk=""
	get_first_disk "2" second_disk
	check_disks $first_disk $second_disk
	clear_after_tests
}

# Run fio test for all devices except one and run separate fio test for this device.
# Check if first fio test passes and second fio test fails.
# Also check if disc has been removed from VM.
# After reboot run fio test for remaining devices and check if fio passes.
function hotdetach_tc4() {
	notice "Hotdetach test case 4"
	sleep 2
	first_disk=""
	get_first_disk "2" first_disk
	prepare_fio_cmd_tc2_iter1 "2"
	$run_fio &
	first_fio_pid=$!
	prepare_fio_cmd_tc3_iter1 "2 3"
	$run_fio &
	second_fio_pid=$!
	sleep 3
	$rpc_py vhost_scsi_controller_remove_target naa.Nvme0n1p4.2 0
	set +xe
	wait $first_fio_pid
	check_fio_retcode "Hotdetach test case 4: Iteration 1." 1 $?
	set -xe
	wait $second_fio_pid
	check_fio_retcode "Hotdetach test case 4: Iteration 2." 0 $?
	second_disk=""
	get_first_disk "2" second_disk
	check_disks $first_disk $second_disk

	reboot_all_and_prepare "2 3"
	sleep 2
	prepare_fio_cmd_tc2_iter2 "2 3"
	$run_fio
	check_fio_retcode "Hotdetach test case 4: Iteration 3." 0 $?
	clear_after_tests
}

function clear_after_tests() {
	$rpc_py vhost_scsi_controller_add_target naa.Nvme0n1p4.2 0 Nvme0n1p8
}

hotdetach_tc1
hotdetach_tc2
hotdetach_tc3
hotdetach_tc4
