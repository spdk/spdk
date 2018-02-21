source $BASE_DIR/autotest.config

MGMT_INITIATOR_IP="10.102.17.180"
RDMA_TARGET_IP="10.0.0.1"
RDMA_INITIATOR_IP="10.0.0.2"
incoming_vm=1
target_vm=2
incoming_vm_ctrl=naa.VhostScsi0.$incoming_vm
target_vm_ctrl=naa.VhostScsi0.$target_vm

function host_2_cleanup_vhost()
{
        notice "Shutting down VM $target_vm"
        vm_kill $target_vm

        notice "Removing bdev & controller from VHOST2"
        $rpc_1 delete_bdev Nvme0n1
        $rpc_1 remove_vhost_controller $target_vm_ctrl

        notice "Shutting down vhost app"
        spdk_vhost_kill 2
        sleep 3
}

function host2_start_vhost()
{
        #rpc_1="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir 1)/rpc.sock"
        rpc_1="python $SPDK_BUILD_DIR/scripts/rpc.py -s /tmp/rpc.sock"
        vhost2_dir=/tmp/vhost2
        mkdir -p $vhost2_dir
        mkdir -p /tmp/share/vhost2
        rm -f /tmp/share/vhost2/*

        notice "Starting VHOST2"
        #vhost_1_reactor_mask=0x1
        #vhost_1_master_core=0
        #spdk_vhost_run --conf-path=$BASE_DIR --vhost-num=1
        trap 'host_2_cleanup_vhost; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT
        /tmp/share/spdk/app/vhost/vhost -r /tmp/rpc.sock -c /tmp/share/spdk/test/vhost/migration/vhost.conf.in -S $vhost2_dir &
        vhost2_pid=$!
        echo $vhost2_pid > /tmp/share/vhost2/vhost.pid
        waitforlisten "$vhost2_pid" "/tmp/rpc.sock"

        $rpc_1 construct_nvme_bdev -b Nvme0 -t rdma -f ipv4 -a $RDMA_TARGET_IP -s 4420 -n "nqn.2018-02.io.spdk:cnode1"
        #$rpc_1 construct_malloc_bdev -b TEST 10 512
        $rpc_1 construct_vhost_scsi_controller $target_vm_ctrl
        $rpc_1 add_vhost_scsi_lun $target_vm_ctrl 0 Nvme0n1
        ln -s $vhost2_dir/naa.VhostScsi0.2 /tmp/share/vhost2/naa.VhostScsi0.2

        vm_setup --os="$os_image" --force=$target_vm --disk-type=spdk_vhost_scsi --disks=VhostScsi0 \
        --memory=512 --vhost-num=2 --incoming=$incoming_vm
        vm_run $target_vm
        sleep 1
        echo "DONE" > /tmp/share/vhost2/DONE
}

host2_start_vhost
$rpc_1 get_bdevs
#read
#host_2_cleanup_vhost
