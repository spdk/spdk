# Vhost blk hot remove tests
#
# Objective
# The purpose of these tests is to verify that SPDK vhost remains stable during
# hot-remove operations performed on SCSI and BLK controllers devices.
# Hot-remove is a scenario where a NVMe device is removed when already in use.
#
# Test cases description
# 1. FIO I/O traffic is run during hot-remove operations.
#    By default FIO uses default_integrity*.job config files located in
#    test/vhost/hotplug/fio_jobs directory.
# 2. FIO mode of operation is random write (randwrite) with verification enabled
#    which results in also performing read operations.
# 3. In test cases fio status is checked after every run if any errors occurred.

function prepare_fio_cmd_tc1() {
	print_test_fio_header

	run_fio="$fio_bin --eta=never "
	for vm_num in $1; do
		cp $fio_job $tmp_detach_job
		vm_check_blk_location $vm_num
		for disk in $SCSI_DISK; do
			echo "[nvme-host$disk]" >> $tmp_detach_job
			echo "filename=/dev/$disk" >> $tmp_detach_job
		done
		vm_scp "$vm_num" $tmp_detach_job 127.0.0.1:/root/default_integrity_2discs.job
		run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity_2discs.job "
		rm $tmp_detach_job
	done
}

function vhost_delete_controllers() {
	$rpc_py vhost_delete_controller naa.Nvme0n1p0.0
	$rpc_py vhost_delete_controller naa.Nvme0n1p1.0
	$rpc_py vhost_delete_controller naa.Nvme0n1p2.1
	$rpc_py vhost_delete_controller naa.Nvme0n1p3.1
}

# Vhost blk hot remove test cases
#
# Test Case 1
function blk_hotremove_tc1() {
	echo "Blk hotremove test case 1"
	traddr=""
	# 1. Run the command to hot remove NVMe disk.
	get_traddr "Nvme0"
	delete_nvme "Nvme0"
	# 2. If vhost had crashed then tests would stop running
	sleep 1
	add_nvme "HotInNvme0" "$traddr"
	sleep 1
}

# Test Case 2
function blk_hotremove_tc2() {
	echo "Blk hotremove test case 2"
	# 1. Use rpc command to create blk controllers.
	$rpc_py vhost_create_blk_controller naa.Nvme0n1p0.0 HotInNvme0n1p0
	$rpc_py vhost_create_blk_controller naa.Nvme0n1p1.0 Mallocp0
	$rpc_py vhost_create_blk_controller naa.Nvme0n1p2.1 Mallocp1
	$rpc_py vhost_create_blk_controller naa.Nvme0n1p3.1 Mallocp2
	# 2. Run two VMs and attach every VM to two blk controllers.
	vm_run_with_arg "0 1"
	vms_prepare "0"

	traddr=""
	get_traddr "Nvme0"
	prepare_fio_cmd_tc1 "0"
	# 3. Run FIO I/O traffic with verification enabled on NVMe disk.
	$run_fio &
	local last_pid=$!
	sleep 3
	# 4. Run the command to hot remove NVMe disk.
	delete_nvme "HotInNvme0"
	local retcode=0
	wait_for_finish $last_pid || retcode=$?
	# 5. Check that fio job run on hot-removed device stopped.
	#    Expected: Fio should return error message and return code != 0.
	check_fio_retcode "Blk hotremove test case 2: Iteration 1." 1 $retcode

	# 6. Reboot VM
	reboot_all_and_prepare "0"
	# 7. Run FIO I/O traffic with verification enabled on NVMe disk.
	$run_fio &
	local retcode=0
	wait_for_finish $! || retcode=$?
	# 8. Check that fio job run on hot-removed device stopped.
	#    Expected: Fio should return error message and return code != 0.
	check_fio_retcode "Blk hotremove test case 2: Iteration 2." 1 $retcode
	vm_shutdown_all
	vhost_delete_controllers
	add_nvme "HotInNvme1" "$traddr"
	sleep 1
}

# ## Test Case 3
function blk_hotremove_tc3() {
	echo "Blk hotremove test case 3"
	# 1. Use rpc command to create blk controllers.
	$rpc_py vhost_create_blk_controller naa.Nvme0n1p0.0 HotInNvme1n1p0
	$rpc_py vhost_create_blk_controller naa.Nvme0n1p1.0 Mallocp0
	$rpc_py vhost_create_blk_controller naa.Nvme0n1p2.1 HotInNvme1n1p1
	$rpc_py vhost_create_blk_controller naa.Nvme0n1p3.1 Mallocp1
	# 2. Run two VMs and attach every VM to two blk controllers.
	vm_run_with_arg "0 1"
	vms_prepare "0 1"

	traddr=""
	get_traddr "Nvme0"
	prepare_fio_cmd_tc1 "0"
	# 3. Run FIO I/O traffic with verification enabled on first NVMe disk.
	$run_fio &
	local last_pid=$!
	sleep 3
	# 4. Run the command to hot remove of first NVMe disk.
	delete_nvme "HotInNvme1"
	local retcode=0
	wait_for_finish $last_pid || retcode=$?
	# 6. Check that fio job run on hot-removed device stopped.
	#    Expected: Fio should return error message and return code != 0.
	check_fio_retcode "Blk hotremove test case 3: Iteration 1." 1 $retcode

	# 7. Reboot VM
	reboot_all_and_prepare "0"
	local retcode=0
	# 8. Run FIO I/O traffic with verification enabled on removed NVMe disk.
	$run_fio &
	wait_for_finish $! || retcode=$?
	# 9. Check that fio job run on hot-removed device stopped.
	#    Expected: Fio should return error message and return code != 0.
	check_fio_retcode "Blk hotremove test case 3: Iteration 2." 1 $retcode
	vm_shutdown_all
	vhost_delete_controllers
	add_nvme "HotInNvme2" "$traddr"
	sleep 1
}

# Test Case 4
function blk_hotremove_tc4() {
	echo "Blk hotremove test case 4"
	# 1. Use rpc command to create blk controllers.
	$rpc_py vhost_create_blk_controller naa.Nvme0n1p0.0 HotInNvme2n1p0
	$rpc_py vhost_create_blk_controller naa.Nvme0n1p1.0 Mallocp0
	$rpc_py vhost_create_blk_controller naa.Nvme0n1p2.1 HotInNvme2n1p1
	$rpc_py vhost_create_blk_controller naa.Nvme0n1p3.1 Mallocp1
	# 2. Run two VM, attached to blk controllers.
	vm_run_with_arg "0 1"
	vms_prepare "0 1"

	prepare_fio_cmd_tc1 "0"
	# 3. Run FIO I/O traffic on first VM with verification enabled on both NVMe disks.
	$run_fio &
	local last_pid_vm0=$!

	prepare_fio_cmd_tc1 "1"
	# 4. Run FIO I/O traffic on second VM with verification enabled on both NVMe disks.
	$run_fio &
	local last_pid_vm1=$!

	sleep 3
	prepare_fio_cmd_tc1 "0 1"
	# 5. Run the command to hot remove of first NVMe disk.
	delete_nvme "HotInNvme2"
	local retcode_vm0=0
	local retcode_vm1=0
	wait_for_finish $last_pid_vm0 || retcode_vm0=$?
	wait_for_finish $last_pid_vm1 || retcode_vm1=$?
	# 6. Check that fio job run on hot-removed device stopped.
	#    Expected: Fio should return error message and return code != 0.
	check_fio_retcode "Blk hotremove test case 4: Iteration 1." 1 $retcode_vm0
	check_fio_retcode "Blk hotremove test case 4: Iteration 2." 1 $retcode_vm1

	# 7. Reboot all VMs.
	reboot_all_and_prepare "0 1"
	# 8. Run FIO I/O traffic with verification enabled on removed NVMe disk.
	$run_fio &
	local retcode=0
	wait_for_finish $! || retcode=$?
	# 9. Check that fio job run on hot-removed device stopped.
	#    Expected: Fio should return error message and return code != 0.
	check_fio_retcode "Blk hotremove test case 4: Iteration 3." 1 $retcode

	vm_shutdown_all
	vhost_delete_controllers
	add_nvme "HotInNvme3" "$traddr"
	sleep 1
}

# Test Case 5
function blk_hotremove_tc5() {
	echo "Blk hotremove test case 5"
	# 1. Use rpc command to create blk controllers.
	$rpc_py vhost_create_blk_controller naa.Nvme0n1p0.0 HotInNvme3n1p0
	$rpc_py vhost_create_blk_controller naa.Nvme0n1p1.0 Mallocp0
	$rpc_py vhost_create_blk_controller naa.Nvme0n1p2.1 Mallocp1
	$rpc_py vhost_create_blk_controller naa.Nvme0n1p3.1 Mallocp2
	# 2. Run two VM, attached to blk controllers.
	vm_run_with_arg "0 1"
	vms_prepare "0 1"

	prepare_fio_cmd_tc1 "0"
	# 3. Run FIO I/O traffic on first VM with verification enabled on both NVMe disks.
	$run_fio &
	local last_pid=$!
	sleep 3
	# 4. Run the command to hot remove of first NVMe disk.
	delete_nvme "HotInNvme3"
	local retcode=0
	wait_for_finish $last_pid || retcode=$?
	# 5. Check that fio job run on hot-removed device stopped.
	#    Expected: Fio should return error message and return code != 0.
	check_fio_retcode "Blk hotremove test case 5: Iteration 1." 1 $retcode

	# 6. Reboot VM.
	reboot_all_and_prepare "0"
	local retcode=0
	# 7. Run FIO I/O traffic with verification enabled on removed NVMe disk.
	$run_fio &
	wait_for_finish $! || retcode=$?
	# 8. Check that fio job run on hot-removed device stopped.
	#    Expected: Fio should return error message and return code != 0.
	check_fio_retcode "Blk hotremove test case 5: Iteration 2." 1 $retcode
	vm_shutdown_all
	vhost_delete_controllers
	add_nvme "HotInNvme4" "$traddr"
	sleep 1
}

vms_setup
blk_hotremove_tc1
blk_hotremove_tc2
blk_hotremove_tc3
blk_hotremove_tc4
blk_hotremove_tc5
