function migration_tc1_clean_vhost_config()
{
	# Restore trap
	trap 'error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT

	notice "Removing vhost devices & controllers via RPC ..."
	# Delete bdev first to remove all LUNs and SCSI targets
	$rpc delete_bdev Malloc0

	# Delete controllers
	$rpc remove_vhost_controller $incoming_vm_ctrlr
	$rpc remove_vhost_controller $target_vm_ctrlr

	unset -v incoming_vm target_vm incoming_vm_ctrlr target_vm_ctrlr rpc
}

function migration_tc1_configure_vhost()
{
	# Those are global intentionaly - they will be unset in cleanup handler
	incoming_vm=0
	target_vm=1
	incoming_vm_ctrlr=naa.Malloc0.$incoming_vm
	target_vm_ctrlr=naa.Malloc0.$target_vm
	rpc="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

	trap 'migration_tc1_error_handler; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT

	# Construct shared Malloc Bdev
	$rpc construct_malloc_bdev -b Malloc0 128 4096

	# And two controllers - one for each VM. Both are using the same Malloc Bdev as LUN 0
	$rpc construct_vhost_scsi_controller $incoming_vm_ctrlr
	$rpc add_vhost_scsi_lun $incoming_vm_ctrlr 0 Malloc0

	$rpc construct_vhost_scsi_controller $target_vm_ctrlr
	$rpc add_vhost_scsi_lun $target_vm_ctrlr 0 Malloc0
}

function migration_tc1_error_handler()
{
	trap - SIGINT ERR EXIT
	set -x

	vm_kill_all
	migration_tc1_clean_vhost_config

	warning "Migration TC1 FAILED"
}

function migration_tc1()
{
	# Use 2 VMs:
	# incoming VM - the one we want to migrate
	# targe VM - the one which will accept migration
	local job_file="$BASE_DIR/migration-tc1.job"

	# Run vhost
	spdk_vhost_run --conf-path=$BASE_DIR
	migration_tc1_configure_vhost

	notice "Setting up VMs"
	vm_setup --os="$os_image" --force=$incoming_vm --disk-type=spdk_vhost_scsi --disks=Malloc0 --migrate-to=$target_vm
	vm_setup --force=$target_vm --disk-type=spdk_vhost_scsi --disks=Malloc0 --incoming=$incoming_vm

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

	migration_tc1_clean_vhost_config

	notice "killing vhost app"
	spdk_vhost_kill

	notice "Migration TC1 SUCCESS"
}

migration_tc1
