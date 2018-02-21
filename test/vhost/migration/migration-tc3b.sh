source $BASE_DIR/autotest.config

MGMT_INITIATOR_IP="10.102.17.180"
RDMA_TARGET_IP="10.0.0.1"
RDMA_INITIATOR_IP="10.0.0.2"
incoming_vm=1
target_vm=2
incoming_vm_ctrl=naa.VhostScsi0.$incoming_vm
target_vm_ctrl=naa.VhostScsi0.$target_vm
rpc_1="python $SPDK_BUILD_DIR/scripts/rpc.py -s /tmp/rpc.sock"

function host_2_cleanup_vhost()
{
		notice "Shutting down VM $target_vm"
		vm_kill $target_vm

		notice "Removing bdev & controller from VHOST2"
		$rpc_1 delete_bdev Nvme0n1
		$rpc_1 remove_vhost_controller $target_vm_ctrl

		notice "Shutting down vhost app"
		spdk_vhost_kill 1
		sleep 1
}

function host_2_start_vhost()
{
		vhost_sock_dir=/tmp/vhost1
		mkdir -p $vhost1_dir
		mkdir -p /tmp/share/vhost1
		rm -f /tmp/share/vhost1/*

		notice "Starting vhost1 instance on local server"
		trap 'host_2_cleanup_vhost; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT
		/tmp/share/spdk/app/vhost/vhost -r /tmp/rpc.sock -c /$BASE_DIR/vhost.conf.in -S /tmp &
		vhost_pid=$!
		echo $vhost_pid > /tmp/share/vhost1/vhost.pid
		waitforlisten "$vhost1_pid" "/tmp/rpc.sock"

		$rpc_1 construct_nvme_bdev -b Nvme0 -t rdma -f ipv4 -a $RDMA_TARGET_IP -s 4420 -n "nqn.2018-02.io.spdk:cnode1"
		$rpc_1 construct_vhost_scsi_controller $target_vm_ctrl
		$rpc_1 add_vhost_scsi_lun $target_vm_ctrl 0 Nvme0n1
		ln -s $vhost1_dir/naa.VhostScsi0.2 /tmp/share/vhost2/naa.VhostScsi0.2

		vm_setup --os="$os_image" --force=$target_vm --disk-type=spdk_vhost_scsi --disks=VhostScsi0 \
		--memory=512 --vhost-num=1 --incoming=$incoming_vm
		vm_run $target_vm
		sleep 1

		# Use this file as a flag to notify main script
		# that setup on remote server is done
		echo "DONE" > /tmp/share/vhost2/DONE
}

host_2_start_vhost
