set -xe

# Vhost SCSI hotremove tests
#
# # Objective
# The purpose of these tests is to verify that SPDK vhost remains stable during
# hot-remove operations performed on SCSI controllers devices.
# Hot-remove is a scenario where a NVMe device is removed when already in use.
# Tests consist of 4 test cases.
#
# # Test cases description
# 1. FIO I/O traffic is run during hot-remove operations.
#    By default FIO uses default_integrity*.job config files located in
#    test/vhost/hotplug/fio_jobs directory.
# 2. FIO mode of operation is random write (randwrite) with verification enabled
#    which results in also performing read operations.

function prepare_fio_cmd_tc1() {
	print_test_fio_header

	run_fio="$fio_bin --eta=never "
	for vm_num in $1; do
		cp $fio_job $tmp_detach_job
		vm_check_scsi_location $vm_num
		for disk in $SCSI_DISK; do
			cat <<- EOL >> $tmp_detach_job
				[nvme-host$disk]
				filename=/dev/$disk
				size=100%
			EOL
		done
		vm_scp "$vm_num" $tmp_detach_job 127.0.0.1:/root/default_integrity_2discs.job
		run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity_2discs.job "
		rm $tmp_detach_job
	done
}

# Vhost SCSI hot-remove test cases.

# Test Case 1
function scsi_hotremove_tc1() {
	echo "Scsi hotremove test case 1"
	traddr=""
	get_traddr "Nvme0"
	# 1. Run the command to hot remove NVMe disk.
	delete_nvme "Nvme0"
	# 2. If vhost had crashed then tests would stop running
	sleep 1
	add_nvme "HotInNvme0" "$traddr"
}

# Test Case 2
function scsi_hotremove_tc2() {
	echo "Scsi hotremove test case 2"
	# 1. Attach split NVMe bdevs to scsi controller.
	$rpc_py vhost_scsi_controller_add_target naa.Nvme0n1p0.0 0 HotInNvme0n1p0
	$rpc_py vhost_scsi_controller_add_target naa.Nvme0n1p1.0 0 Mallocp0
	$rpc_py vhost_scsi_controller_add_target naa.Nvme0n1p2.1 0 HotInNvme0n1p1
	$rpc_py vhost_scsi_controller_add_target naa.Nvme0n1p3.1 0 Mallocp1

	# 2. Run two VMs, attached to scsi controllers.
	vms_setup
	vm_run_with_arg 0 1
	vms_prepare "0 1"

	vm_check_scsi_location "0"
	local disks="$SCSI_DISK"

	traddr=""
	get_traddr "Nvme0"
	prepare_fio_cmd_tc1 "0 1"
	# 3. Run FIO I/O traffic with verification enabled on on both NVMe disks in VM.
	$run_fio &
	local last_pid=$!
	sleep 3
	# 4. Run the command to hot remove NVMe disk.
	delete_nvme "HotInNvme0"

	# 5. Check that fio job run on hot-remove device stopped on VM.
	#    Expected: Fio should return error message and return code != 0.
	wait_for_finish $last_pid || retcode=$?
	check_fio_retcode "Scsi hotremove test case 2: Iteration 1." 1 $retcode

	# 6. Check if removed devices are gone from VM.
	vm_check_scsi_location "0"
	local new_disks="$SCSI_DISK"
	check_disks "$disks" "$new_disks"
	# 7. Reboot both VMs.
	reboot_all_and_prepare "0 1"
	# 8. Run FIO I/O traffic with verification enabled on on both VMs.
	local retcode=0
	$run_fio &
	wait_for_finish $! || retcode=$?
	# 9. Check that fio job run on hot-remove device stopped on both VMs.
	#     Expected: Fio should return error message and return code != 0.
	check_fio_retcode "Scsi hotremove test case 2: Iteration 2." 1 $retcode
	vm_shutdown_all
	add_nvme "HotInNvme1" "$traddr"
	sleep 1
}

# Test Case 3
function scsi_hotremove_tc3() {
	echo "Scsi hotremove test case 3"
	# 1. Attach added NVMe bdev to scsi controller.
	$rpc_py vhost_scsi_controller_add_target naa.Nvme0n1p0.0 0 HotInNvme1n1p0
	# 2. Run two VM, attached to scsi controllers.
	vm_run_with_arg 0 1
	vms_prepare "0 1"
	vm_check_scsi_location "0"
	local disks="$SCSI_DISK"
	traddr=""
	get_traddr "Nvme0"
	# 3. Run FIO I/O traffic with verification enabled on on both NVMe disks in VMs.
	prepare_fio_cmd_tc1 "0"
	$run_fio &
	local last_pid=$!
	sleep 3
	# 4. Run the command to hot remove NVMe disk.
	delete_nvme "HotInNvme1"
	# 5. Check that fio job run on hot-remove device stopped on first VM.
	#    Expected: Fio should return error message and return code != 0.
	wait_for_finish $last_pid || retcode=$?
	check_fio_retcode "Scsi hotremove test case 3: Iteration 1." 1 $retcode
	# 6. Check if removed devices are gone from lsblk.
	vm_check_scsi_location "0"
	local new_disks="$SCSI_DISK"
	check_disks "$disks" "$new_disks"
	# 7. Reboot both VMs.
	reboot_all_and_prepare "0 1"
	# 8. Run FIO I/O traffic with verification enabled on on both VMs.
	local retcode=0
	$run_fio &
	wait_for_finish $! || retcode=$?
	# 9. Check that fio job run on hot-remove device stopped on both VMs.
	#    Expected: Fio should return error message and return code != 0.
	check_fio_retcode "Scsi hotremove test case 3: Iteration 2." 1 $retcode
	vm_shutdown_all
	add_nvme "HotInNvme2" "$traddr"
	sleep 1
}

# Test Case 4
function scsi_hotremove_tc4() {
	echo "Scsi hotremove test case 4"
	# 1. Attach NVMe bdevs to scsi controllers.
	$rpc_py vhost_scsi_controller_add_target naa.Nvme0n1p0.0 0 HotInNvme2n1p0
	$rpc_py vhost_scsi_controller_add_target naa.Nvme0n1p2.1 0 HotInNvme2n1p1
	# 2. Run two VMs, attach to scsi controller.
	vm_run_with_arg 0 1
	vms_prepare "0 1"

	# 3. Run FIO I/O traffic with verification enabled on first VM.
	vm_check_scsi_location "0"
	local disks_vm0="$SCSI_DISK"
	# 4. Run FIO I/O traffic with verification enabled on second VM.
	prepare_fio_cmd_tc1 "0"
	$run_fio &
	last_pid_vm0=$!

	vm_check_scsi_location "1"
	local disks_vm1="$SCSI_DISK"
	prepare_fio_cmd_tc1 "1"
	$run_fio &
	local last_pid_vm1=$!
	prepare_fio_cmd_tc1 "0 1"
	sleep 3
	# 5. Run the command to hot remove NVMe disk.
	traddr=""
	get_traddr "Nvme0"
	delete_nvme "HotInNvme2"
	# 6. Check that fio job run on hot-removed devices stopped.
	#    Expected: Fio should return error message and return code != 0.
	local retcode_vm0=0
	wait_for_finish $last_pid_vm0 || retcode_vm0=$?
	local retcode_vm1=0
	wait_for_finish $last_pid_vm1 || retcode_vm1=$?
	check_fio_retcode "Scsi hotremove test case 4: Iteration 1." 1 $retcode_vm0
	check_fio_retcode "Scsi hotremove test case 4: Iteration 2." 1 $retcode_vm1

	# 7. Check if removed devices are gone from lsblk.
	vm_check_scsi_location "0"
	local new_disks_vm0="$SCSI_DISK"
	check_disks "$disks_vm0" "$new_disks_vm0"
	vm_check_scsi_location "1"
	local new_disks_vm1="$SCSI_DISK"
	check_disks "$disks_vm1" "$new_disks_vm1"

	# 8. Reboot both VMs.
	reboot_all_and_prepare "0 1"
	# 9. Run FIO I/O traffic with verification enabled on on not-removed NVMe disk.
	local retcode=0
	$run_fio &
	wait_for_finish $! || retcode=$?
	# 10. Check that fio job run on hot-removed device stopped.
	#    Expect: Fio should return error message and return code != 0.
	check_fio_retcode "Scsi hotremove test case 4: Iteration 3." 1 $retcode
	prepare_fio_cmd_tc1 "0 1"
	# 11. Run FIO I/O traffic with verification enabled on on not-removed NVMe disk.
	local retcode=0
	$run_fio &
	wait_for_finish $! || retcode=$?
	# 12. Check finished status FIO. Write and read in the not-removed.
	#     NVMe disk should be successful.
	#     Expected: Fio should return return code == 0.
	check_fio_retcode "Scsi hotremove test case 4: Iteration 4." 0 $retcode
	vm_shutdown_all
	add_nvme "HotInNvme3" "$traddr"
	sleep 1
	$rpc_py vhost_scsi_controller_remove_target naa.Nvme0n1p1.0 0
	$rpc_py vhost_scsi_controller_remove_target naa.Nvme0n1p3.1 0
}

function pre_scsi_hotremove_test_case() {
	$rpc_py vhost_create_scsi_controller naa.Nvme0n1p0.0
	$rpc_py vhost_create_scsi_controller naa.Nvme0n1p1.0
	$rpc_py vhost_create_scsi_controller naa.Nvme0n1p2.1
	$rpc_py vhost_create_scsi_controller naa.Nvme0n1p3.1
}

function post_scsi_hotremove_test_case() {
	$rpc_py vhost_delete_controller naa.Nvme0n1p0.0
	$rpc_py vhost_delete_controller naa.Nvme0n1p1.0
	$rpc_py vhost_delete_controller naa.Nvme0n1p2.1
	$rpc_py vhost_delete_controller naa.Nvme0n1p3.1
}

pre_scsi_hotremove_test_case
scsi_hotremove_tc1
scsi_hotremove_tc2
scsi_hotremove_tc3
scsi_hotremove_tc4
post_scsi_hotremove_test_case
