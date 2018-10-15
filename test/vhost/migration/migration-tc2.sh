source $SPDK_BUILD_DIR/test/nvmf/common.sh

function migration_tc2_cleanup_nvmf_tgt()
{
	local i

	if [[ ! -r "$nvmf_dir/nvmf_tgt.pid" ]]; then
		warning "Pid file '$nvmf_dir/nvmf_tgt.pid' does not exist. "
		return
	fi

	if [[ ! -z "$1" ]]; then
		trap 'error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT
		pkill --signal $1 -F $nvmf_dir/nvmf_tgt.pid || true
		sleep 5
		if ! pkill -F $nvmf_dir/nvmf_tgt.pid; then
			fail "failed to kill nvmf_tgt app"
		fi
	else
		pkill --signal SIGTERM -F $nvmf_dir/nvmf_tgt.pid || true
		for (( i=0; i<20; i++ )); do
			if ! pkill --signal 0 -F $nvmf_dir/nvmf_tgt.pid; then
				break
			fi
			sleep 0.5
		done

		if pkill --signal 0 -F $nvmf_dir/nvmf_tgt.pid; then
			error "nvmf_tgt failed to shutdown"
		fi
	fi

	rm $nvmf_dir/nvmf_tgt.pid
	unset -v nvmf_dir rpc_nvmf
}

function migration_tc2_cleanup_vhost_config()
{
	timing_enter migration_tc2_cleanup_vhost_config

	trap 'migration_tc2_cleanup_nvmf_tgt SIGKILL; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT

	notice "Shutting down all VMs"
	vm_shutdown_all

	notice "Removing vhost devices & controllers via RPC ..."
	# Delete bdev first to remove all LUNs and SCSI targets
	$rpc_0 delete_nvme_controller Nvme0
	$rpc_0 remove_vhost_controller $incoming_vm_ctrlr

	$rpc_1 delete_nvme_controller Nvme0
	$rpc_1 remove_vhost_controller $target_vm_ctrlr

	notice "killing vhost app"
	spdk_vhost_kill 0
	spdk_vhost_kill 1

	unset -v incoming_vm target_vm incoming_vm_ctrlr target_vm_ctrlr
	unset -v rpc_0 rpc_1

	trap 'error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT
	migration_tc2_cleanup_nvmf_tgt

	timing_exit migration_tc2_cleanup_vhost_config
}

function migration_tc2_configure_vhost()
{
	timing_enter migration_tc2_configure_vhost

	# Those are global intentionally - they will be unset in cleanup handler
	nvmf_dir="$TEST_DIR/nvmf_tgt"

	incoming_vm=1
	target_vm=2
	incoming_vm_ctrlr=naa.VhostScsi0.$incoming_vm
	target_vm_ctrlr=naa.VhostScsi0.$target_vm

	rpc_nvmf="$SPDK_BUILD_DIR/scripts/rpc.py -s $nvmf_dir/rpc.sock"
	rpc_0="$SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"
	rpc_1="$SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir 1)/rpc.sock"

	# Default cleanup/error handlers will not shutdown nvmf_tgt app so setup it
	# here to teardown in cleanup function
	trap 'migration_tc2_error_cleanup; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT

	# Run nvmf_tgt and two vhost instances:
	# nvmf_tgt uses core id 2 (-m 0x4)
	# First uses core id 0 (vhost_0_reactor_mask=0x1)
	# Second uses core id 1 (vhost_1_reactor_mask=0x2)
	# This force to use VM 1 and 2.
	timing_enter start_nvmf_tgt
	notice "Running nvmf_tgt..."
	mkdir -p $nvmf_dir
	rm -f $nvmf_dir/*
	$SPDK_BUILD_DIR/app/nvmf_tgt/nvmf_tgt -s 512 -m 0x4 -r $nvmf_dir/rpc.sock --wait-for-rpc &
	local nvmf_tgt_pid=$!
	echo $nvmf_tgt_pid > $nvmf_dir/nvmf_tgt.pid
	waitforlisten "$nvmf_tgt_pid" "$nvmf_dir/rpc.sock"
	$rpc_nvmf start_subsystem_init
	$rpc_nvmf nvmf_create_transport -t RDMA -u 8192 -p 4
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh --json | $rpc_nvmf load_subsystem_config
	timing_exit start_nvmf_tgt

	spdk_vhost_run --memory=512 --vhost-num=0 --no-pci
	# Those are global intentionally
	vhost_1_reactor_mask=0x2
	vhost_1_master_core=1
	spdk_vhost_run --memory=512 --vhost-num=1 --no-pci

	local rdma_ip_list=$(get_available_rdma_ips)
	local nvmf_target_ip=$(echo "$rdma_ip_list" | head -n 1)

	if [[ -z "$nvmf_target_ip" ]]; then
		fail "no NIC for nvmf target"
	fi

	notice "Configuring nvmf_tgt, vhost devices & controllers via RPC ..."

	# Construct shared bdevs and controllers
	$rpc_nvmf nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
	$rpc_nvmf nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Nvme0n1
	$rpc_nvmf nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t rdma -a $nvmf_target_ip -s 4420

	$rpc_0 construct_nvme_bdev -b Nvme0 -t rdma -f ipv4 -a $nvmf_target_ip -s 4420 -n "nqn.2016-06.io.spdk:cnode1"
	$rpc_0 construct_vhost_scsi_controller $incoming_vm_ctrlr
	$rpc_0 add_vhost_scsi_lun $incoming_vm_ctrlr 0 Nvme0n1

	$rpc_1 construct_nvme_bdev -b Nvme0 -t rdma -f ipv4 -a $nvmf_target_ip -s 4420 -n "nqn.2016-06.io.spdk:cnode1"
	$rpc_1 construct_vhost_scsi_controller $target_vm_ctrlr
	$rpc_1 add_vhost_scsi_lun $target_vm_ctrlr 0 Nvme0n1

	notice "Setting up VMs"
	vm_setup --os="$os_image" --force=$incoming_vm --disk-type=spdk_vhost_scsi --disks=VhostScsi0 \
		--migrate-to=$target_vm  --memory=1024 --vhost-num=0
	vm_setup --force=$target_vm --disk-type=spdk_vhost_scsi --disks=VhostScsi0 --incoming=$incoming_vm --memory=1024 \
		--vhost-num=1

	# Run everything
	vm_run $incoming_vm $target_vm

	# Wait only for incoming VM, as target is waiting for migration
	vm_wait_for_boot 600 $incoming_vm

	notice "Configuration done"

	timing_exit migration_tc2_configure_vhost
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
	local job_file="$MIGRATION_DIR/migration-tc2.job"

	migration_tc2_configure_vhost

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

	migration_tc2_cleanup_vhost_config
	notice "Migration TC2 SUCCESS"
}

migration_tc2
