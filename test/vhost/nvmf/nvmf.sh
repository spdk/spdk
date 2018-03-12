#!/usr/bin/env bash
set -e
. $(readlink -e "$(dirname $0)/../common/common.sh") || exit 1

disk="Nvme0n1"
ctrl_type="spdk_vhost_scsi"
use_virtio_initiator=false

rm -f $TEST_DIR/nvmf.conf $TEST_DIR/bdev.conf $TEST_DIR/virtio.job

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Usage: $(basename $1) [-h|--help] [--fiobin=PATH]"
	echo "-h, --help                      Print help and exit"
	echo "    --vm-image=PATH             Path to VM image used in these tests"
	echo "    --fio-bin=PATH              Use specific fio binary (will be uploaded to VM)"
	echo "    --fio-job=PATH              Fio config to use for test"
	echo "    --with-malloc               Use malloc bdev"
	echo "    --with-vhost-blk            Create vhost block controller instead scsi controller"
	echo "    --with-virtio-initiator     Test on vhost virtio initiator using fio_plugin"
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0; exit 0 ;;
			fio-bin=*) fio_bin="${OPTARG#*=}" ;;
			vm-image=*) vm_path="${OPTARG#*=}" ;;
			fio-job=*) fio_jobs="${OPTARG#*=}" ;;
			with-malloc) disk="malloc" ;;
			with-vhost-blk) ctrl_type="spdk_vhost_blk" ;;
			with-virtio-initiator) use_virtio_initiator=true ;;
			*) usage $0 echo "Invalid argument '$OPTARG'"; exit 1 ;;
		esac
		;;
		h) usage $0; exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'"; exit 1 ;;
	esac
done

. $SPDK_BUILD_DIR/test/nvmf/common.sh

if [[ $EUID -ne 0 ]]; then
	fail "INFO: Go away user come back as root"
fi

if [ ! -x $fio_bin ]; then
	fail "Invalid path of fio binary"
fi

if [[ ! -r "$fio_jobs" ]]; then
	fail "no fio job file specified"
fi

function kill_nvmf_tgt()
{
	if [[ ! -r $TEST_DIR/nvmf_tgt.pid ]]; then
		warning "no nvmf_tg pid file found"
		return 0
	fi

	local nvmf_tgt_pid="$(cat $TEST_DIR/nvmf_tgt.pid)"
	if /bin/kill -INT $nvmf_tgt_pid; then
		for ((i=0; i<60; i++)); do
			if /bin/kill -0 $nvmf_tgt_pid; then
				echo "Waiting nvmf_tgt app to exit"
				sleep 1
			else
				break
			fi
		done
		if /bin/kill -0 $nvmf_tgt_pid; then
			error "nvmf_tgt did not exited. Sending SIGTERM"
			/bin/kill -SIGTERM $nvmf_tgt_pid
		fi
	fi

	rm -f $TEST_DIR/nvmf_tgt.pid $TEST_DIR/nvmf.conf $TEST_DIR/bdev.conf $TEST_DIR/virtio.job
}

function nvmf_error_exit()
{
	#terminate vhost and VMs
	at_app_exit
	/bin/kill -SIGTERM $nvmf_tgt_pid
	trap - ERR
	rm -f $TEST_DIR/nvmf_tgt.pid $TEST_DIR/nvmf.conf $TEST_DIR/bdev.conf $TEST_DIR/virtio.job
	print_backtrace
	set +e
	error "Error on $1 $2"
	exit 1
}

trap 'nvmf_error_exit "${FUNCNAME}" "${LINENO}"' ERR

function run_spdk_fio_virtio() {
	LD_PRELOAD=$SPDK_BUILD_DIR/examples/bdev/fio_plugin/fio_plugin /usr/src/fio/fio --ioengine=spdk_bdev\
         "$@" --spdk_mem=1024
}

set -x
rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"
rpc_nvmf="python $SPDK_BUILD_DIR/scripts/rpc.py -s $TEST_DIR/rpc_nvmf.sock"

timing_enter spdk_nvmf_tgt
cp $SPDK_BUILD_DIR/test/nvmf/nvmf.conf $TEST_DIR/nvmf.conf
$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $TEST_DIR/nvmf.conf

$SPDK_BUILD_DIR/app/nvmf_tgt/nvmf_tgt -s 1024 -m 0x2 -c $TEST_DIR/nvmf.conf -r $TEST_DIR/rpc_nvmf.sock &
nvmf_tgt_pid=$!
echo $nvmf_tgt_pid > $TEST_DIR/nvmf_tgt.pid

waitforlisten "$nvmf_tgt_pid" "$TEST_DIR/rpc_nvmf.sock"
timing_exit spdk_nvmf_tgt

timing_enter bdev_setup
rdma_ip_list=$(get_available_rdma_ips)
nvmf_target_ip=$(echo "$rdma_ip_list" | head -n 1)

if [[ $disk == "malloc" ]]; then
	disk="$($rpc_nvmf construct_malloc_bdev 128 512)"
fi

$rpc_nvmf construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 \
	"trtype:RDMA traddr:$nvmf_target_ip trsvcid:4420" "" -a -s SPDK00000000000001 -n "$disk"

timing_enter spdk_vhost_run
spdk_vhost_run --conf-path=$BASE_DIR
timing_exit spdk_vhost_run

$rpc_py construct_nvme_bdev -b $disk -t rdma -f ipv4 -a $nvmf_target_ip -s 4420 -n "nqn.2016-06.io.spdk:cnode1"
$rpc_py get_bdevs
timing_exit bdev_setup

vm_no="0"
disk=$($rpc_py get_bdevs | jq -r '.[].name')

if [[ $ctrl_type = "spdk_vhost_scsi" ]]; then
	$rpc_py construct_vhost_scsi_controller naa.$disk.$vm_no
	$rpc_py add_vhost_scsi_lun naa.$disk.$vm_no 0 $disk
else
	$rpc_py construct_vhost_blk_controller naa.$disk.$vm_no $disk
fi

$rpc_py get_vhost_controllers
if $use_virtio_initiator; then
	timing_enter virtio_initiator_setup
	touch $TEST_DIR/bdev.conf
	echo "[VirtioUser0]" >> $TEST_DIR/bdev.conf
	echo "  Path $(get_vhost_dir)/naa.$disk.$vm_no" >> $TEST_DIR/bdev.conf
	echo "  Queues 2" >> $TEST_DIR/bdev.conf

	vbdevs=$(discover_bdevs $SPDK_BUILD_DIR $TEST_DIR/bdev.conf)
	virtio_bdevs=$(jq -r '[.[].name] | join(":")' <<< $vbdevs)
	sed "s@filename=@filename=$virtio_bdevs@" $fio_jobs >> $TEST_DIR/virtio.job
	timing_exit virtio_initiator_setup

	timing_enter run_spdk_fio_virtio
	run_spdk_fio_virtio $TEST_DIR/virtio.job --spdk_conf=$TEST_DIR/bdev.conf
	timing_exit run_spdk_fio_virtio
else
	timing_enter vm_setup
	vm_setup --disk-type=$ctrl_type --force=$vm_no --os=$vm_path --disks=$disk
	vm_run $vm_no
	vm_wait_for_boot 600 $vm_no
	timing_exit vm_setup

	vm_dir=$VM_BASE_DIR/$vm_no
	qemu_mask_param="VM_${vm_no}_qemu_mask"
	host_name="VM-$vm_no-${!qemu_mask_param}"
	notice "Setting up hostname: $host_name"
	vm_ssh $vm_no "hostname $host_name"
	vm_start_fio_server --fio-bin=$fio_bin $readonly $vm_no
	if [[ $ctrl_type = "spdk_vhost_scsi" ]]; then
		vm_check_scsi_location $vm_no
	else
		vm_check_blk_location $vm_no
	fi

	fio_disks+=" --vm=$vm_no$(printf ':/dev/%s' $SCSI_DISK)"

	timing_enter run_fio
	run_fio --fio-bin=$fio_bin --job-file="$fio_jobs" --out="$TEST_DIR/fio_results" $fio_disks
	timing_exit run_fio

	set +x
	echo "========================================================================================================"
	echo "Printing dmesg from guest"
	echo "========================================================================================================"

	vm_ssh $vm_no "dmesg"
	echo "========================================================================================================"
	echo "End of dmesg"
	echo "========================================================================================================"
	set -x
fi

timing_enter vm_shutdown_all
vm_shutdown_all
timing_exit vm_shutdown_all

timing_enter spdk_vhost_kill
spdk_vhost_kill
timing_exit spdk_vhost_kill

timing_enter kill_nvmf_tgt
kill_nvmf_tgt
timing_exit kill_nvmf_tgt
