source $BASE_DIR/autotest.config

target_vm=2
target_vm_ctrl=naa.VhostScsi0.$target_vm
rpc="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir 1)/rpc.sock"

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
