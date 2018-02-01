function migration_tc2_cleanup_vhost_config()
{
	trap 'error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT

	notice "Removing vhost devices & controllers via RPC ..."
	# Delete bdev first to remove all LUNs and SCSI targets
	$rpc_0 delete_bdev Aio0
	$rpc_0 remove_vhost_controller $incoming_vm_ctrlr
	
	$rpc_1 delete_bdev Aio0
	$rpc_1 remove_vhost_controller $target_vm_ctrlr
	
	rm $migration_tc2_test_file

	unset -v incoming_vm target_vm incoming_vm_ctrlr target_vm_ctrlr rpc_0 rpc_1 mitration_tc2_test_file
}

function migration_tc2_configure_vhost()
{
	trap 'migration_tc2_error_cleanup; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT
	
	# Those are global intentionaly - they will be unset in cleanup handler
	incoming_vm=0
	target_vm=1
	incoming_vm_ctrlr=naa.Aio0.$incoming_vm
	target_vm_ctrlr=naa.Aio0.$target_vm
	rpc_0="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir $incoming_vm)/rpc.sock"
	rpc_1="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir $target_vm)/rpc.sock"
	
	notice "Configuring vhost devices & controllers via RPC ..."

	#Configure NVMeoF
	migration_tc2_test_file=$TEST_DIR/migration_tc2_Aio_file.raw
	truncate -s $(( 128 * 1024 * 1024 )) $migration_tc2_test_file
	
	# Construct shared bdevs and controllers
	$rpc_0 construct_aio_bdev $migration_tc2_test_file Aio0 4096
	$rpc_0 construct_vhost_scsi_controller $incoming_vm_ctrlr
	$rpc_0 add_vhost_scsi_lun $incoming_vm_ctrlr 0 Aio0
	
	$rpc_1 construct_aio_bdev $migration_tc2_test_file Aio0 4096
	$rpc_1 construct_vhost_scsi_controller $target_vm_ctrlr
	$rpc_1 add_vhost_scsi_lun $target_vm_ctrlr 0 Aio0
	
	notice "Configuration done"
}

function migration_tc2_error_cleanup()
{
	trap - SIGINT ERR EXIT
	set -x

	vm_kill_all
	migration_tc2_cleanup_vhost_config
	notice "Migration TC2 FAILED"
}

function migration_tc2()
{
	# Use 2 VMs:
	# incoming VM - the one we want to migrate
	# targe VM - the one which will accept migration
	local job_file="$BASE_DIR/migration-tc2.job"
	
	# Run two instance of vhost
	spdk_vhost_run --conf-path=$BASE_DIR --vhost-num=0
	
	# Second vhost uses core id 13 to not run on the same one
	vhost_1_reactor_mask=0x2000
	vhost_1_master_core=13
	spdk_vhost_run --conf-path=$BASE_DIR --vhost-num=1

	migration_tc2_configure_vhost
	
	notice "Setting up VMs"
	vm_setup --os="$os_image" --force=$incoming_vm --disk-type=spdk_vhost_scsi --disks=Aio0 --migrate-to=$target_vm --vhost-num=0
	vm_setup --force=$target_vm --disk-type=spdk_vhost_scsi --disks=Aio0 --incoming=$incoming_vm --vhost-num=1
	
	# Run everything
	vm_run $incoming_vm $target_vm
	
	# Wait only for incoming VM, as target is waiting for migration
	vm_wait_for_boot 600 $incoming_vm
	
	# Run fio before migration
	notice "Starting FIO"
	vm_check_scsi_location $incoming_vm
	run_fio $fio_bin --job-file="$job_file" --local --vm="${incoming_vm}$(printf ':/dev/%s' $SCSI_DISK)"
	
	# Wait a while to let the FIO time to issue some IO
	sleep 5
	
	# Check if fio is still running before migration
	if ! is_fio_running $incoming_vm; then
		vm_ssh $incoming_vm "cat /root/$(basename ${job_file}).out"
		error "FIO is not running before migration: process crashed or finished too early"
	fi
	
	vm_migrate $incoming_vm
	sleep 3
	
	# Check if fio is still running after migration
	if ! is_fio_running $target_vm; then
		vm_ssh $target_vm "cat /root/$(basename ${job_file}).out"
		error "FIO is not running after migration: process crashed or finished too early"
	fi
	
	notice "Waiting for fio to finish"
	local timeout=40
	while is_fio_running $target_vm; do
		sleep 1
		echo -n "."
		if (( timeout-- == 0 )); then
			error "timeout while waiting for FIO!"
		fi
	done
	
	notice "Fio result is:"
	vm_ssh $target_vm "cat /root/$(basename ${job_file}).out"
	
	notice "Migration DONE"
	
	notice "Shutting down all VMs"
	vm_shutdown_all
	migration_tc2_cleanup_vhost_config
	
	notice "killing vhost app"
	spdk_vhost_kill 0 1
	
	notice "Migration TC2 SUCCESS"
}
migration_tc2
