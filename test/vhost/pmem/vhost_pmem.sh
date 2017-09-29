#!/usr/bin/env bash
set -ex
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"

. $COMMON_DIR/common.sh

ctrl_type="spdk_vhost_scsi"
PMEM_SIZE=128
PMEM_BLOCK_SIZE=512
RPC_PORT=5260
rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py"
LOOP_RANGE=1
used_vms=""
vm_count=1
pmem_per_vm=1

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for doing automated hotattach/hotdetach test"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "-h, --help                Print help and exit"
	echo "    --vm-count=INT        Virtual machines to use in test;"
	echo "                          Default: 1"
	echo "    --pmem-per-vm=INT     Pmem bdevs per VM;"
	echo "                          Default: 1"
	echo "    --ctrl-type=TYPE      Controller type to use for test:"
	echo "                              spdk_vhost_scsi - use spdk vhost scsi"
	echo "                              spdk_vhost_blk - use spdk vhost block"
	echo "    --fio-bin=FIO         Use specific fio binary (will be uploaded to VM)"
	echo "    --work-dir=WORK_DIR   Where to find build file. Must exist. [default: $TEST_DIR]"
	echo "    --os=OS.qcow2         OS - VM os disk path (optional)"
	exit 0
}

while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 ;;
			vm-count=*) vm_count="${OPTARG#*=}" ;;
			pmem-per-vm=*) pmem_per_vm="${OPTARG#*=}" ;;
			work-dir=*) ROOT_DIR="${OPTARG#*=}" ;;
			ctrl-type=*) ctrl_type="${OPTARG#*=}" ;;
			fio-bin=*) fio_bin="${OPTARG#*=}" ;;
			os=*) os="${OPTARG#*=}" ;;
			*) usage $0 "Invalid argument '$OPTARG'" ;;
		esac
		;;
	h) usage $0 ;;
	*) usage $0 "Invalid argument '$OPTARG'"
	esac
done
shift $(( OPTIND - 1 ))

$COMMON_DIR/run_vhost.sh $x --work-dir=$TEST_DIR --conf-dir=$BASE_DIR

trap "vm_shutdown_all; spdk_vhost_kill; rm -f /tmp/pool_file*; exit 1" SIGINT SIGTERM EXIT

for (( i=0; i<vm_count; i++)); do
	setup_cmd="$COMMON_DIR/vm_setup.sh $x --work-dir=$TEST_DIR"
	setup_cmd+=" --test-type=$ctrl_type -f $i --os=$os"

	bdevs=()
	for (( j=0; j<pmem_per_vm; j++)); do
		$rpc_py create_pmem_pool /tmp/pool_file_${i}_${j} $PMEM_SIZE $PMEM_BLOCK_SIZE
		pmem_bdev="$($rpc_py construct_pmem_bdev /tmp/pool_file_${i}_${j})"
		bdevs+=("$pmem_bdev")
	done

	if [[ "$ctrl_type" == "spdk_vhost_scsi" ]]; then
		$rpc_py construct_vhost_scsi_controller naa.$i.$i
		for (( j=0; j<pmem_per_vm; j++)); do
			$rpc_py add_vhost_scsi_lun naa.$i.$i $j ${bdevs[$j]}
		done
		setup_cmd+=" --disk=$i"
	elif [[ "$ctrl_type" == "spdk_vhost_blk" ]]; then
		disk=""
		for (( j=0; j<pmem_per_vm; j++)); do
			$rpc_py construct_vhost_blk_controller naa.$j.$i ${bdevs[$j]}
			disk+="${j}_size_120M:"
		done
		disk="${disk::-1}"
		setup_cmd+=" --disk=$disk"
	fi

	$setup_cmd
	if [[ ! -d "${TEST_DIR}/vms/${i}" ]]; then
		echo "VM $i is not properly prepared"
		break
	fi
	used_vms+=" $i"
done

$rpc_py get_vhost_controllers
$rpc_py get_luns

$COMMON_DIR/vm_run.sh $x --work-dir=$TEST_DIR $used_vms
vm_wait_for_boot 600 $used_vms

# Get disk names from VMs and run FIO traffic
run_fio="python $COMMON_DIR/run_fio.py --fio-bin=$fio_bin"
run_fio+=" --job-file=$BASE_DIR/pmem_integrity.job"
run_fio+=" --out=$TEST_DIR "

for vm_num in $used_vms; do
	vm_dir=$VM_BASE_DIR/$vm_num
	qemu_mask_param="VM_${vm_num}_qemu_mask"

	host_name="VM-$vm_num-${!qemu_mask_param}"
	vm_ssh $vm_num "hostname $host_name"
	vm_start_fio_server --fio-bin=$fio_bin $vm_num

	if [[ "$ctrl_type" == "spdk_vhost_scsi" ]]; then
		vm_check_scsi_location $vm_num
	elif [[ "$ctrl_type" == "spdk_vhost_blk" ]]; then
		vm_check_blk_location $vm_num
	fi

	run_fio+="127.0.0.1:$(cat $vm_dir/fio_socket):"
	for disk in $SCSI_DISK; do
		run_fio+="/dev/$disk:"
	done
	run_fio="${run_fio::-1}"
	run_fio+=","
done
run_fio="${run_fio::-1}"

# Run FIO traffic
echo -e "$run_fio"
$run_fio

vm_shutdown_all

for (( i=0; i<vm_count; i++)); do
	for (( j=0; j<pmem_per_vm; j++)); do
		if [[ "$ctrl_type" == "spdk_vhost_scsi" ]]; then
			$rpc_py remove_vhost_scsi_dev naa.$i.$i $j
		elif [[ "$ctrl_type" == "spdk_vhost_blk" ]]; then
			$rpc_py remove_vhost_controller naa.$j.$i
		fi
	done

	if [[ "$ctrl_type" == "spdk_vhost_scsi" ]]; then
		$rpc_py remove_vhost_controller naa.$i.$i
	fi
done

spdk_vhost_kill

trap - SIGINT SIGTERM EXIT

rm -f /tmp/pool_file*
