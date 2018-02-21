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
share_dir=/tmp/share
job_file=$BASE_DIR/migration-tc3.job

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
	error "IP $RDMA_TARGET_IP is not set on any RDMA capable NIC on local server"
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

	notice "Starting nvmf_tgt instance on local server"
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

	notice "Starting vhost0 instance on local server"
	trap 'host1_cleanup_vhost; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT
	spdk_vhost_run --conf-path=$BASE_DIR --vhost-num=0
	$rpc_0 construct_nvme_bdev -b Nvme0 -t rdma -f ipv4 -a $RDMA_TARGET_IP -s 4420 -n "nqn.2018-02.io.spdk:cnode1"
	$rpc_0 construct_vhost_scsi_controller $incoming_vm_ctrlr
	$rpc_0 add_vhost_scsi_lun $incoming_vm_ctrlr 0 Nvme0n1

	vm_setup --os="/tmp/share/migration.qcow2" --force=$incoming_vm --disk-type=spdk_vhost_scsi --disks=VhostScsi0 \
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
	ssh_remote $MGMT_INITIATOR_IP "umount $share_dir; rm -f $share_dir/*"
	rm -f $share_dir/migration.qcow2
	rm -f $share_dir/spdk.tar.gz
	set -e
}

function host_1_create_share(){
	notice "Creating share directory on local server to re-use on remote"
	mkdir -p $share_dir
	rm -f $share_dir/spdk.tar.gz || true
	rm -rf $share_dir/spdk || true
	cp $os_image $share_dir/migration.qcow2
	tar --exclude="*.o"--exclude="*.d" --exclude="*.git" -C $SPDK_BUILD_DIR -zcf $share_dir/spdk.tar.gz .
}

function host_2_create_share(){
	# Copy & compile the sources for later use on remote server.
	ssh_remote $MGMT_INITIATOR_IP "uname -a"
	ssh_remote $MGMT_INITIATOR_IP "mkdir -p $share_dir"
	ssh_remote $MGMT_INITIATOR_IP "sshfs -o ssh_command=\"ssh -i $SPDK_VHOST_SSH_KEY_FILE\" root@$MGMT_TARGET_IP:$share_dir $share_dir"
	ssh_remote $MGMT_INITIATOR_IP "mkdir -p $share_dir/spdk"
	ssh_remote $MGMT_INITIATOR_IP "tar -zxf $share_dir/spdk.tar.gz -C $share_dir/spdk --strip-components=1"
	ssh_remote $MGMT_INITIATOR_IP "cd $share_dir/spdk; make clean; ./configure --with-rdma --enable-debug; make -j40"
}

function setup_share()
{
	trap 'cleanup_share; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT
	host_1_create_share
	host_2_create_share
}

check_rdma_connection
setup_share
host1_start_nvmf
host1_start_vhost

ssh_remote $MGMT_INITIATOR_IP "nohup bash -c '$share_dir/spdk/test/vhost/migration/migration.sh --test-cases=3b --work-dir=$share_dir/ --os=/tmp/share/migration.qcow2' 1>/tmp/share/stdout.log 2>/tmp/share/stderr.log"
notice "Waiting for remote to be done with vhost & VM setup..."
while [[ ! -f /tmp/share/vhost1/DONE ]]; do
	echo "."
done

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

ssh_remote $MGMT_INITIATOR_IP "$share_dir//spdk/test/vhost/migration/migration.sh --test-cases=3c --work-dir=$share_dir"
host1_cleanup_vhost
cleanup_share
