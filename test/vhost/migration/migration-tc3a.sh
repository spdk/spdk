source $SPDK_BUILD_DIR/test/nvmf/common.sh
source $MIGRATION_DIR/autotest.config

incoming_vm=1
target_vm=2
incoming_vm_ctrlr=naa.VhostScsi0.$incoming_vm
target_vm_ctrlr=naa.VhostScsi0.$target_vm
share_dir=$TEST_DIR/share
spdk_repo_share_dir=$TEST_DIR/share_spdk
job_file=$MIGRATION_DIR/migration-tc3.job

if [ -z "$MGMT_TARGET_IP" ]; then
	error "No IP address of target is given"
fi

if [ -z "$MGMT_INITIATOR_IP" ]; then
	error "No IP address of initiator  is given"
fi

if [ -z "$RDMA_TARGET_IP" ]; then
	error "No IP address of targets RDMA capable NIC is given"
fi

if [ -z "$RDMA_INITIATOR_IP" ]; then
	error "No IP address of initiators RDMA capable NIC is given"
fi

function ssh_remote()
{
	local ssh_cmd="ssh -i $SPDK_VHOST_SSH_KEY_FILE \
		-o UserKnownHostsFile=/dev/null \
		-o StrictHostKeyChecking=no -o ControlMaster=auto \
		root@$1"

	shift
	$ssh_cmd "$@"
}

function wait_for_remote()
{
	local timeout=40
	set +x
	while [[ ! -f $share_dir/DONE ]]; do
		echo -n "."
		if (( timeout-- == 0 )); then
			error "timeout while waiting for FIO!"
		fi
		sleep 1
	done
	set -x
	rm -f $share_dir/DONE
}

function check_rdma_connection()
{
	local nic_name=$(ip -4 -o addr show to $RDMA_TARGET_IP up | cut -d' ' -f2)
	if [[ -z $nic_name ]]; then
		error "There is no NIC with IP address $RDMA_TARGET_IP configured"
	fi

	if ! ls /sys/class/infiniband/*/device/net/$nic_name &> /dev/null; then
		error "$nic_name with IP $RDMA_TARGET_IP is not a RDMA capable NIC"
	fi

}

function host1_cleanup_nvmf()
{
	notice "Shutting down nvmf_tgt on local server"
	if [[ ! -z "$1" ]]; then
		pkill --signal $1 -F $nvmf_dir/nvmf_tgt.pid
	else
		pkill -F $nvmf_dir/nvmf_tgt.pid
	fi
	rm -f $nvmf_dir/nvmf_tgt.pid
}

function host1_cleanup_vhost()
{
	trap 'host1_cleanup_nvmf SIGKILL; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT
	notice "Shutting down VM $incoming_vm"
	vm_kill $incoming_vm

	notice "Removing bdev & controller from vhost on local server"
	$rpc_0 delete_nvme_controller Nvme0
	$rpc_0 remove_vhost_controller $incoming_vm_ctrlr

	notice "Shutting down vhost app"
	spdk_vhost_kill 0

	host1_cleanup_nvmf
}

function host1_start_nvmf()
{
	nvmf_dir="$TEST_DIR/nvmf_tgt"
	rpc_nvmf="$SPDK_BUILD_DIR/scripts/rpc.py -s $nvmf_dir/nvmf_rpc.sock"

	notice "Starting nvmf_tgt instance on local server"
	mkdir -p $nvmf_dir
	rm -rf $nvmf_dir/*

	trap 'host1_cleanup_nvmf SIGKILL; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT
	$SPDK_BUILD_DIR/app/nvmf_tgt/nvmf_tgt -s 512 -m 0xF -r $nvmf_dir/nvmf_rpc.sock --wait-for-rpc &
	nvmf_tgt_pid=$!
	echo $nvmf_tgt_pid > $nvmf_dir/nvmf_tgt.pid
	waitforlisten "$nvmf_tgt_pid" "$nvmf_dir/nvmf_rpc.sock"
	$rpc_nvmf start_subsystem_init
	$rpc_nvmf nvmf_create_transport -t RDMA -u 8192 -p 4
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh --json | $rpc_nvmf load_subsystem_config

	$rpc_nvmf nvmf_subsystem_create nqn.2018-02.io.spdk:cnode1 -a -s SPDK01
	$rpc_nvmf nvmf_subsystem_add_ns nqn.2018-02.io.spdk:cnode1 Nvme0n1
	$rpc_nvmf nvmf_subsystem_add_listener nqn.2018-02.io.spdk:cnode1 -t rdma -a $RDMA_TARGET_IP -s 4420
}

function host1_start_vhost()
{
	rpc_0="$SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"

	notice "Starting vhost0 instance on local server"
	trap 'host1_cleanup_vhost; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT
	spdk_vhost_run --vhost-num=0 --no-pci
	$rpc_0 construct_nvme_bdev -b Nvme0 -t rdma -f ipv4 -a $RDMA_TARGET_IP -s 4420 -n "nqn.2018-02.io.spdk:cnode1"
	$rpc_0 construct_vhost_scsi_controller $incoming_vm_ctrlr
	$rpc_0 add_vhost_scsi_lun $incoming_vm_ctrlr 0 Nvme0n1

	vm_setup --os="$share_dir/migration.qcow2" --force=$incoming_vm --disk-type=spdk_vhost_scsi --disks=VhostScsi0 \
		--migrate-to=$target_vm --memory=512 --queue_num=1

	# TODO: Fix loop calculating cpu_num in common.sh
	# We need -smp 1 and -queue_num 1 for this test to work, and this loop
	# in some cases calculates wrong cpu_num.
	sed -i "s#smp 2#smp 1#g" $VM_BASE_DIR/$incoming_vm/run.sh
	vm_run $incoming_vm
	vm_wait_for_boot 300 $incoming_vm
}

function cleanup_share()
{
	set +e
	notice "Cleaning up share directory on remote and local server"
	ssh_remote $MGMT_INITIATOR_IP "umount $VM_BASE_DIR"
	ssh_remote $MGMT_INITIATOR_IP "umount $share_dir; rm -f $share_dir/* rm -rf $spdk_repo_share_dir"
	rm -f $share_dir/migration.qcow2
	rm -f $share_dir/spdk.tar.gz
	set -e
}

function host_1_create_share()
{
	notice "Creating share directory on local server to re-use on remote"
	mkdir -p $share_dir
	mkdir -p $VM_BASE_DIR # This dir would've been created later but we need it now
	rm -rf $share_dir/spdk.tar.gz $share_dir/spdk || true
	cp $os_image $share_dir/migration.qcow2
	tar --exclude="*.o"--exclude="*.d" --exclude="*.git" -C $SPDK_BUILD_DIR -zcf $share_dir/spdk.tar.gz .
}

function host_2_create_share()
{
	# Copy & compile the sources for later use on remote server.
	ssh_remote $MGMT_INITIATOR_IP "uname -a"
	ssh_remote $MGMT_INITIATOR_IP "mkdir -p $share_dir"
	ssh_remote $MGMT_INITIATOR_IP "mkdir -p $spdk_repo_share_dir"
	ssh_remote $MGMT_INITIATOR_IP "mkdir -p $VM_BASE_DIR"
	ssh_remote $MGMT_INITIATOR_IP "sshfs -o\
	 ssh_command=\"ssh -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ControlMaster=auto\
	 -i $SPDK_VHOST_SSH_KEY_FILE\" root@$MGMT_TARGET_IP:$VM_BASE_DIR $VM_BASE_DIR"
	ssh_remote $MGMT_INITIATOR_IP "sshfs -o\
	 ssh_command=\"ssh -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o ControlMaster=auto\
	 -i $SPDK_VHOST_SSH_KEY_FILE\" root@$MGMT_TARGET_IP:$share_dir $share_dir"
	ssh_remote $MGMT_INITIATOR_IP "mkdir -p $spdk_repo_share_dir/spdk"
	ssh_remote $MGMT_INITIATOR_IP "tar -zxf $share_dir/spdk.tar.gz -C $spdk_repo_share_dir/spdk --strip-components=1"
	ssh_remote $MGMT_INITIATOR_IP "cd $spdk_repo_share_dir/spdk; make clean; ./configure --with-rdma --enable-debug; make -j40"
}

function host_2_start_vhost()
{
	ssh_remote $MGMT_INITIATOR_IP "nohup $spdk_repo_share_dir/spdk/test/vhost/migration/migration.sh\
	 --test-cases=3b --work-dir=$TEST_DIR --os=$share_dir/migration.qcow2\
	 --rdma-tgt-ip=$RDMA_TARGET_IP &>$share_dir/output.log &"
	notice "Waiting for remote to be done with vhost & VM setup..."
	wait_for_remote
}

function setup_share()
{
	trap 'cleanup_share; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT
	host_1_create_share
	host_2_create_share
}

function migration_tc3()
{
	check_rdma_connection
	setup_share
	host1_start_nvmf
	host1_start_vhost
	host_2_start_vhost

	# Do migration
	notice "Starting fio on local VM"
	vm_check_scsi_location $incoming_vm

	run_fio $fio_bin --job-file="$job_file" --local --vm="${incoming_vm}$(printf ':/dev/%s' $SCSI_DISK)"
	sleep 5

	if ! is_fio_running $incoming_vm; then
		vh_ssh $incoming_vm "cat /root/$(basename ${job_file}).out"
		error "Fio not running on local VM before starting migration!"
	fi

	vm_migrate $incoming_vm $RDMA_INITIATOR_IP
	sleep 1

	# Verify migration on remote host and clean up vhost
	ssh_remote $MGMT_INITIATOR_IP "pkill -CONT -F $TEST_DIR/tc3b.pid"
	notice "Waiting for remote to finish FIO on VM and clean up..."
	wait_for_remote

	# Clean up local stuff
	host1_cleanup_vhost
	cleanup_share
}

migration_tc3
