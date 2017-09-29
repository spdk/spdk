#!/usr/bin/env bash
set -ex
BASE_DIR=$(readlink -f $(dirname $0))
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

PMEM_BDEVS_LIST=""
PMEM_SIZE=128
PMEM_BLOCK_SIZE=512
SUBSYS_NR=10

function vm_create_ssh_config()
{
	local ssh_config="$VM_BASE_DIR/ssh_config"
	if [[ ! -f $ssh_config ]]; then
		(
		echo "Host *"
		echo "  ControlPersist=10m"
		echo "  ConnectTimeout=2"
		echo "  Compression=no"
		echo "  ControlMaster=auto"
		echo "  UserKnownHostsFile=/dev/null"
		echo "  StrictHostKeyChecking=no"
		echo "  User root"
		echo "  ControlPath=$VM_BASE_DIR/%r@%h:%p.ssh"
		echo ""
		) > $ssh_config
	fi
}

function vm_scp()
{
	vm_num_is_valid $1 || return 1
	vm_create_ssh_config
	local ssh_config="$VM_BASE_DIR/ssh_config"

	local scp_cmd="scp -i $SPDK_VHOST_SSH_KEY_FILE -F $ssh_config \
		-P $(vm_ssh_socket $1) "

	shift
	$scp_cmd "$@"
}

function prepare_fio() {
	tmp_job=$BASE_DIR/fio.job.tmp
    run_fio="$TEST_DIR/fio_universal --eta=never "
    for vm_num in $1; do
        vm_dir=$VM_BASE_DIR/$vm_num
        vm_check_blk_location $vm_num
        for disk in $SCSI_DISK; do
			echo "[global]"
			echo "ioengine=libaio"
			echo "size=120M"
			echo "io_size=10G"
			echo "filename=/dev/$disk"
			echo "numjobs=1"
			echo "bs=4k"
			echo "iodepth=128"
			echo "direct=1"
			echo "rw=randread"
			echo "group_reporting"
			echo "thread"
			echo "[nvme-host]"
        done >> $tmp_job
        vm_scp "$vm_num" $tmp_job 127.0.0.1:/root/default_integrity_discs.job
        run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity_discs.job "
        rm $tmp_job
    done
}

function vms_prepare() {
    for vm_num in `seq 0 $SUBSYS_NR`; do
        vm_dir=$VM_BASE_DIR/$vm_num

        qemu_mask_param="VM_${vm_num}_qemu_mask"

        host_name="VM-${vm_num}-${!qemu_mask_param}"
        echo "INFO: Setting up hostname: $host_name"
        vm_ssh $vm_num "hostname $host_name"
        vm_start_fio_server --fio-bin=$TEST_DIR/fio_universal $ls  $vm_num
    done
}

function clear_pmem_pool()
{
	$rpc_py remove_vhost_controller naa.pmem0.0
	for pmem in $PMEM_BDEVS; do
		$rpc_py delete_bdev $pmem
	done

	for i in `seq 0 $SUBSYS_NR`; do
		$rpc_py delete_pmem_pool /tmp/pool_file$i
	done
}


rpc_py="python $ROOT_DIR/scripts/rpc.py"

. $BASE_DIR/../common/common.sh

$BASE_DIR/../common/run_vhost.sh $x --work-dir=$TEST_DIR --conf-dir=$BASE_DIR &
pid=$!
waitforlisten $pid

timing_start vhost_pmem

trap "vm_shutdown_all; spdk_vhost_kill;  rm -f /tmp/pool_file*; exit 1" SIGINT SIGTERM EXIT

for i in `seq 0 $SUBSYS_NR`; do
		$rpc_py create_pmem_pool /tmp/pool_file$i $PMEM_SIZE $PMEM_BLOCK_SIZE
		PMEM_BDEV="$($rpc_py construct_pmem_bdev /tmp/pool_file$i)"
		$rpc_py construct_vhost_blk_controller naa.$PMEM_BDEVS.0 $PMEM_BDEV	
		sleep 1
		$BASE_DIR/../common/vm_setup.sh $x --work-dir=$TEST_DIR --test-type=spdk_vhost_blk --os=$TEST_DIR/vhost_vm_image.qcow2 --disk=$PMEM_BDEV
		$BASE_DIR/../common/vm_run.sh $x --work-dir=$TEST_DIR $i
		vm_wait_for_boot 600 $i
done

vms_prepare

for i in `seq 0 $SUBSYS_NR`; do
	prepare_fio $i
	$run_fio &
done

trap - SIGINT SIGTERM EXIT

vm_shutdown_all

rm -f ./local-job*
clear_pmem_pool
spdk_vhost_kill
timing_exit vhost_pmem
