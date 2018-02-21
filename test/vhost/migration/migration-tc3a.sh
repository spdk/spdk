source $SPDK_BUILD_DIR/test/nvmf/common.sh
source $BASE_DIR/autotest.config

MGMT_TARGET_IP="10.102.17.181"
MGMT_INITIATOR_IP="10.102.17.180"
RDMA_TARGET_IP="10.0.0.1"
RDMA_INITIATOR_IP="10.0.0.2" 
incoming_vm=1
target_vm=2
incoming_vm_ctrlr=naa.VhostScsi0.$incoming_vm
target_vm_ctrlr=naa.VhostScsi0.$target_vm

function ssh_remote()
{
	local ssh_cmd="ssh -i $SPDK_VHOST_SSH_KEY_FILE \
		-o UserKnownHostsFile=/dev/null \
		-o StrictHostKeyChecking=no -o ControlMaster=auto \
		root@$1"

	shift
	$ssh_cmd "$@"
}

function check_rdma_connection()
{
	for nic_type in `ls /sys/class/infiniband`; do
		for nic_name in `ls /sys/class/infiniband/${nic_type}/device/net`; do
			if [[ $(cat /sys/class/infiniband/${nic_type}/device/net/${nic_name}/operstate) == "up" ]];then
				if ip -4 -o addr show dev ${nic_name} | grep $RDMA_TARGET_IP; then
					return 0
				fi
			fi
		done
	done
	error "IP $RDMA_TARGET_IP is not set on any RDMA capable NIC"
}

function host1_cleanup_nvmf()
{
	notice "Shutting down NVMF_TGT"
	if [[ ! -z "$1" ]]; then
		pkill --signal $1 -F $nvmf_dir/nvmf_tgt.pid
	else
		pkill -F $nvmf_dir/nvmf_tgt.pid
	fi
	#rm -f $nvmf_dir/nvmf_tgt.pid
}

function host1_cleanup_vhost()
{
	trap 'host1_cleanup_nvmf SIGKILL; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT
	notice "Shutting down VM $incoming_vm"
	vm_kill $incoming_vm
	
	notice "Removing bdev & controller from VHOST0"
	$rpc_0 delete_bdev Nvme0n1
	$rpc_0 remove_vhost_controller $incoming_vm_ctrlr
	
	notice "Shutting down vhost app"
	spdk_vhost_kill 0
	
	host1_cleanup_nvmf
}

function host1_start_nvmf()
{
	nvmf_dir="$TEST_DIR/nvmf_tgt"
	rpc_nvmf="python $SPDK_BUILD_DIR/scripts/rpc.py -s $nvmf_dir/nvmf_rpc.sock"
	
	notice "Starting NVMF_TGT"
	mkdir -p $nvmf_dir
	rm -rf $nvmf_dir/*
	
	cp $SPDK_BUILD_DIR/test/nvmf/nvmf.conf $nvmf_dir/nvmf.conf
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $nvmf_dir/nvmf.conf
	
	trap 'host1_cleanup_nvmf SIGKILL; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT
	$SPDK_BUILD_DIR/app/nvmf_tgt/nvmf_tgt -s 512 -c $nvmf_dir/nvmf.conf -r $nvmf_dir/nvmf_rpc.sock &
	nvmf_tgt_pid=$!
	echo $nvmf_tgt_pid > $nvmf_dir/nvmf_tgt.pid
	waitforlisten "$nvmf_tgt_pid" "$nvmf_dir/nvmf_rpc.sock"

	$rpc_nvmf construct_nvmf_subsystem nqn.2018-02.io.spdk:cnode1 \
		"trtype:RDMA traddr:$RDMA_TARGET_IP trsvcid:4420" "" -a -s SPDK01 -n Nvme0n1
}

function host1_start_vhost()
{
	rpc_0="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"
	
	notice "Starting VHOST0"
	trap 'host1_cleanup_vhost; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT
	spdk_vhost_run --conf-path=$BASE_DIR --vhost-num=0
	$rpc_0 construct_nvme_bdev -b Nvme0 -t rdma -f ipv4 -a $RDMA_TARGET_IP -s 4420 -n "nqn.2018-02.io.spdk:cnode1"
	$rpc_0 construct_vhost_scsi_controller $incoming_vm_ctrlr 
	$rpc_0 add_vhost_scsi_lun $incoming_vm_ctrlr 0 Nvme0n1
	
	vm_setup --os="/tmp/share/migration.qcow2" --force=$incoming_vm --disk-type=spdk_vhost_scsi --disks=VhostScsi0 \
			--migrate-to=$target_vm --memory=512 --queue_num=1
	sed -i "s#smp 2#smp 1#g" $VM_BASE_DIR/$incoming_vm/run.sh
	vm_run $incoming_vm
	vm_wait_for_boot 300 $incoming_vm
}

function cleanup_share()
{
	echo "X"
}

function create_share_host_1(){
	# On Host 1 create /tmp/share directory and put a test VM image in there.
	share_dir="/tmp/share"
	mkdir -p $share_dir
	rm -f $share_dir/spdk.tar.gz || true
	rm -rf $share_dir/spdk || true
	cp $os_image /tmp/share/migration.qcow2
	echo "SPDK_BUILD: $SPDK_BUILD_DIR"
	tar --exclude="*.o"--exclude="*.d" --exclude="*.git" -C $SPDK_BUILD_DIR -zcf $share_dir/spdk.tar.gz .
}

function create_share_host_2(){
	# Copy & compile the sources
	ssh_remote $MGMT_INITIATOR_IP "uname -a"
	ssh_remote $MGMT_INITIATOR_IP "mkdir -p $share_dir"
	ssh_remote $MGMT_INITIATOR_IP "sshfs -o ssh_command=\"ssh -i $SPDK_VHOST_SSH_KEY_FILE\" root@$MGMT_TARGET_IP:$share_dir $share_dir"
	ssh_remote $MGMT_INITIATOR_IP "mkdir -p $share_dir/spdk"
	ssh_remote $MGMT_INITIATOR_IP "tar -zxf $share_dir/spdk.tar.gz -C $share_dir/spdk --strip-components=1"
	ssh_remote $MGMT_INITIATOR_IP "cd $share_dir/spdk; make clean; ./configure --with-rdma --enable-debug; make -j40"
	#ssh_remote $MGMT_INITIATOR_IP "umount $share_dir"
}

function migration_tc3()
{
	echo "A"
}

function setup_share()
{
	create_share_host_1
	create_share_host_2
}

function migration_tc3()
{
	setup_share
	start_nvmf
	start_vhost
	start_remote_vhost
	#migrate
	#check
	#cleanup
}

#check_rdma_connection
create_share_host_1
create_share_host_2
host1_start_nvmf
host1_start_vhost

ssh_remote $MGMT_INITIATOR_IP "nohup bash -c '/tmp/share/spdk/test/vhost/migration/migration.sh --test-cases=3b --work-dir=/tmp/share --os=/tmp/share/migration.qcow2' 1>/tmp/share/stdout.log 2>/tmp/share/stderr.log"
#setup_host_1
#setup_host_2

echo "HERE"
while [[ ! -f /tmp/share/vhost2/DONE ]]; do
	echo "WAITING FOR REMOTE!!!"
done

notice "STARTING FIO"
vm_check_scsi_location $incoming_vm
job_file=$BASE_DIR/migration-tc3.job
run_fio $fio_bin --job-file="$job_file" --local --vm="${incoming_vm}$(printf ':/dev/%s' $SCSI_DISK)"
sleep 5

if ! is_fio_running $incoming_vm; then
	vh_ssh $incoming_vm "cat /root/$(basename ${job_file}).out"
	echo "ERROR: FIO NOT RUNNING BEFORE MIGRATION"
fi


vm_migrate $incoming_vm $RDMA_INITIATOR_IP 
echo "HERE"
sleep 2
read
ssh_remote $MGMT_INITIATOR_IP "/tmp/share/spdk/test/vhost/migration/migration.sh --test-cases=3c --work-dir=/tmp/share"
host1_cleanup_vhost




# On Host 2 create /tmp/share directory. Using SSHFS mount /tmp/share from Host 1

# Start SPDK NVMeOF Target application on Host 1.
# Construct a single NVMe bdev from available bound NVMe drives.
# Create NVMeoF subsystem with NVMe bdev as single namespace.

# Start first SDPK Vhost application instance on Host 1(later referred to as "Vhost_1").
# Use different shared memory ID and CPU mask than NVMeOF Target.
# Construct a NVMe bdev by connecting to NVMeOF Target
# Construct a single SCSI controller and add NVMe bdev to it.

# Start first VM (VM_1) and connect to Vhost_1 controller. Verify if attached disk is visible in the system.


# Start second SDPK Vhost application instance on Host 2(later referred to as "Vhost_2").
# Construct a NVMe bdev by connecting to NVMeOF Target. Connect to the same subsystem as Vhost_1, multiconnection is allowed.
# Construct a single SCSI controller and add NVMe bdev to it.

# Start second VM (VM_2) but with "-incoming" option enabled.

# Check states of both VMs using Qemu monitor utility.
# VM_1 should be in running state.
# VM_2 should be in paused (inmigrate) state.

# Run FIO I/O traffic with verification enabled on to attached NVME on VM_1.
# While FIO is running issue a command for VM_1 to migrate.


# When the migrate call returns check the states of VMs again.
# VM_1 should be in paused (postmigrate) state. "info migrate" should report
# "Migration status: completed".
# VM_2 should be in running state.


# Verify that FIO task completed successfully on VM_2 after migrating.
# There should be no I/O failures, no verification failures, etc.

# Cleanup:
# Shutdown both VMs.
# Gracefully shutdown Vhost instances and NVMEoF Target instance.
# Remove /tmp/share directory and it's contents.
# Clean RDMA NIC configuration.
