# Set -m option is needed to be able to use "suspend" command
# as we are usin non-interactive session to connect to remote.
# Without -m it would be not possible to suspend the process.
set -m
source $BASE_DIR/autotest.config

RDMA_TARGET_IP="10.0.0.1"
incoming_vm=1
target_vm=2
target_vm_ctrl=naa.VhostScsi0.$target_vm
rpc="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir 1)/rpc.sock"
share_dir=$TEST_DIR/share

function host_2_cleanup_vhost()
{
	notice "Shutting down VM $target_vm"
	vm_kill $target_vm

	notice "Removing bdev & controller from vhost 1 on remote server"
	$rpc delete_bdev Nvme0n1
	$rpc remove_vhost_controller $target_vm_ctrl

	notice "Shutting down vhost app"
	spdk_vhost_kill 1
	sleep 1
}

function host_2_start_vhost()
{
	echo "BASE DIR $TEST_DIR"
	vhost_work_dir=$TEST_DIR/vhost1
	mkdir -p $vhost_work_dir
	rm -f $vhost_work_dir/*

	notice "Starting vhost 1 instance on remote server"
	trap 'host_2_cleanup_vhost; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT
	spdk_vhost_run --conf-path=$BASE_DIR --vhost-num=1

	$rpc construct_nvme_bdev -b Nvme0 -t rdma -f ipv4 -a $RDMA_TARGET_IP -s 4420 -n "nqn.2018-02.io.spdk:cnode1"
	$rpc construct_vhost_scsi_controller $target_vm_ctrl
	$rpc add_vhost_scsi_lun $target_vm_ctrl 0 Nvme0n1

	vm_setup --os="$os_image" --force=$target_vm --disk-type=spdk_vhost_scsi --disks=VhostScsi0 \
		--memory=512 --vhost-num=1 --incoming=$incoming_vm
	vm_run $target_vm
	sleep 1

	# Use this file as a flag to notify main script
	# that setup on remote server is done
	echo "DONE" > $share_dir/DONE
}

echo $$ > $TEST_DIR/tc3b.pid
host_2_start_vhost
suspend -f

if ! vm_os_booted $target_vm; then
	fail "VM$target_vm is not running!"
fi

if ! is_fio_running $target_vm; then
	vm_ssh $target_vm "cat /root/migration-tc3.job.out"
	error "FIO is not running on remote server after migration!"
fi

notice "Waiting for FIO to finish on remote server VM"
timeout=40
while is_fio_running $target_vm; do
	sleep 1
	echo -n "."
	if (( timeout-- == 0 )); then
		error "timeout while waiting for FIO!"
	fi
done

notice "FIO result after migration:"
vm_ssh $target_vm "cat /root/migration-tc3.job.out"

host_2_cleanup_vhost
echo "DONE" > $share_dir/DONE
