BASE_DIR=$(readlink -f $(dirname $0))

function run_host_fio() {
		local fio_bdev_conf=$1
		local fio_job_conf=""
		case "$2" in
			"unmap_default") fio_job_conf="--section=fio_job_unmap_trim_sequential --section=fio_job_unmap_trim_random --section=fio_job_unmap_write" ;;
			"4G_default") fio_job_conf="--section=fio_job_4G_randwrite" ;;
			"4G_nightly") fio_job_conf="--section=fio_job_4G_randwrite --section=fio_job_4G_randrw --section=fio_job_4G_write --section=fio_job_4G_rw" ;;
			"host_default") fio_job_conf="--section=fio_job_host_randwrite" ;;
			"host_nightly") fio_job_conf="--section=fio_job_host_randrw --section=fio_job_host_randwrite --section=fio_job_host_rw --section=fio_job_host_write" ;;
		esac

		LD_PRELOAD=$PLUGIN_DIR/fio_plugin $fio_bin --ioengine=spdk_bdev \
			--runtime=10 $BASE_DIR/fio_jobs.fio $fio_job_conf "$fio_bdev_conf" --spdk_mem=1024
		rm -f *.state
		rm -f $BASE_DIR/bdev.fio
}

function start_and_prepare_vm() {
		local os="$os_image"
		local test_type="spdk_vhost_scsi"
		local disk="Nvme0n1"
		local force_vm_num="0"
		local vm_num="0"
		local fio_bin="root/fio_src/fio"
		local setup_cmd="$COMMON_DIR/vm_setup.sh $x --work-dir=$TEST_DIR --test-type=$test_type"
		$COMMON_DIR/vm_setup.sh $x --work-dir=$TEST_DIR --test-type=$test_type --os=$os --disk=$disk -f $force_vm_num --memory=6144 --queue_num=16

		echo "used_vms: $vm_num"
		$COMMON_DIR/vm_run.sh $x --work-dir=$TEST_DIR $vm_num
		vm_wait_for_boot 600 $vm_num
		vm_scp $vm_num -r $ROOT_DIR "127.0.0.1:/root/spdk"
		vm_ssh $vm_num " cd spdk ; make clean ; ./configure --with-fio=/root/fio_src ; make -j3"
		qemu_mask_param="VM_${vm_num}_qemu_mask"
		host_name="VM-$vm_num-${!qemu_mask_param}"
		echo "INFO: Setting up hostname: $host_name"
		vm_ssh $vm_num "hostname $host_name"
		vm_start_fio_server --fio-bin=$fio_bin $vm_num
}

function run_guest_bdevio() {
		local vm_num="0"
		local conf_file="$BASE_DIR/bdevvm.conf"
		vm_scp $vm_num  $conf_file "127.0.0.1:/root/bdev.conf"
		timing_enter bounds
		vm_ssh $vm_num "/root/spdk/scripts/setup.sh ; /root/spdk/test/lib/bdev/bdevio/bdevio /root/bdev.conf"
		timing_exit bounds
}

function run_guest_fio() {
		local fio_bdev_conf=$1
		local vm_num="0"
		local fio_job="$BASE_DIR/fio_jobs.in.fio"
		vm_scp $vm_num $fio_job "127.0.0.1:/root/bdev.fio"
		if [[ $RUN_NIGHTLY -eq 1 ]]; then
			vm_ssh $vm_num "/root/spdk/test/vhost/initiator/guest_fio.sh $fio_bdev_conf --nigthly"
		else
			vm_ssh $vm_num "/root/spdk/test/vhost/initiator/guest_fio.sh $fio_bdev_conf"
		fi
		vm_ssh $vm_num "rm -f *.state"
}

function on_error_exit() {
		set +e
		echo "Error on $1 - $2"
		print_backtrace
		vm_shutdown_all
		spdk_vhost_kill
		exit 1
}
