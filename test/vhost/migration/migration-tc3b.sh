source $BASE_DIR/autotest.config

RDMA_TARGET_IP="10.0.0.1"
incoming_vm=1
target_vm=2
target_vm_ctrl=naa.VhostScsi0.$target_vm
rpc="python $SPDK_BUILD_DIR/scripts/rpc.py -s /tmp/rpc.sock"

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
		$SPDK_BUILD_DIR/app/vhost/vhost -r /tmp/rpc.sock -c $BASE_DIR/vhost.conf.in -S /tmp &
		vhost_pid=$!
		echo $vhost_pid > $vhost_work_dir/vhost.pid
		waitforlisten "$vhost_pid" "/tmp/rpc.sock"

		$rpc construct_nvme_bdev -b Nvme0 -t rdma -f ipv4 -a $RDMA_TARGET_IP -s 4420 -n "nqn.2018-02.io.spdk:cnode1"
		$rpc construct_vhost_scsi_controller $target_vm_ctrl
		$rpc add_vhost_scsi_lun $target_vm_ctrl 0 Nvme0n1
		ln -s /tmp/$target_vm_ctrl $vhost_work_dir/$target_vm_ctrl

		vm_setup --os="$os_image" --force=$target_vm --disk-type=spdk_vhost_scsi --disks=VhostScsi0 \
			--memory=512 --vhost-num=1 --incoming=$incoming_vm
		vm_run $target_vm
		sleep 1

		# Use this file as a flag to notify main script
		# that setup on remote server is done
		echo "DONE" > $vhost_work_dir/DONE
}

host_2_start_vhost
