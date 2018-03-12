#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

fio_bin="/home/sys_sgsw/fio_ubuntu"
vm_path="/home/sys_sgsw/vhost_vm_image.qcow2"
fio_jobs="$COMMON_DIR/fio_jobs/default_integrity.job"
disk="Nvme0n1"
x=""

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Usage: $(basename $1) [-h|--help] [--fiobin=PATH]"
	echo "-h, --help            Print help and exit"
	echo "    --vm_image=PATH   Path to VM image used in these tests [default=$vm_path]"
	echo "    --fiobin=PATH     Use specific fio binary (will be uploaded to VM) [default=$fio_bin]"
	echo "    --fio-job=PATH    Fio config to use for test [default=$fio_jobs]"
}

while getopts 'h-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 && exit 0 ;;
			fiobin=*) fio_bin="${OPTARG#*=}" ;;
			vm_image=*) vm_path="${OPTARG#*=}" ;;
			fio-job=*) fio_jobs="${OPTARG#*=}" ;;
			*) usage $0 echo "Invalid argument '$OPTARG'" && exit 1 ;;
		esac
		;;
		h) usage $0 && exit 0 ;;
		*) usage $0 "Invalid argument '$optchar'" && exit 1 ;;
	esac
done

. $COMMON_DIR/common.sh
. $SPDK_BUILD_DIR/test/nvmf/common.sh

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
	if /bin/kill -INT $nvmf_tgt_pid >/dev/null; then
		for ((i=0; i<60; i++)); do
			if /bin/kill -0 $nvmf_tgt_pid; then
				echo "Waiting nvmf_tgt app to exit"
				sleep 1
			else
				break
			fi
		done
		if /bin/kill -0 $nvmf_tgt_pid; then
			/bin/kill -SIGTERM $nvmf_tgt_pid
		fi
	fi

	rm $TEST_DIR/nvmf_tgt.pid
}

function nvmf_error_exit()
{
	at_app_exit
	kill_nvmf_tgt
	trap - ERR
	print_backtrace
	set +e
	error "Error on $1 $2"
	exit 1
}

trap 'nvmf_error_exit "${FUNCNAME}" "${LINENO}"' ERR

rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"
rpc_nvmf="python $SPDK_BUILD_DIR/scripts/rpc.py -s $TEST_DIR/rpc.sock"
vm_no="0"

cp $SPDK_BUILD_DIR/test/nvmf/nvmf.conf $TEST_DIR/nvmf.conf
$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $TEST_DIR/nvmf.conf
$SPDK_BUILD_DIR/app/nvmf_tgt/nvmf_tgt -s 512 -m 0x2 -c $TEST_DIR/nvmf.conf -r $TEST_DIR/rpc.sock &
nvmf_tgt_pid=$!
echo $nvmf_tgt_pid > $TEST_DIR/nvmf_tgt.pid
waitforlisten "$nvmf_tgt_pid" "$TEST_DIR/rpc.sock"

spdk_vhost_run --conf-path=$BASE_DIR

rdma_ip_list=$(get_available_rdma_ips)
nvmf_target_ip=$(echo "$rdma_ip_list" | head -n 1)
$rpc_nvmf construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 \
	"trtype:RDMA traddr:$nvmf_target_ip trsvcid:4420" "" -a -s SPDK00000000000001 -n Nvme0n1
$rpc_py construct_nvme_bdev -b Nvme0 -t rdma -f ipv4 -a $nvmf_target_ip -s 4420 -n "nqn.2016-06.io.spdk:cnode1"
$rpc_py get_bdevs
$rpc_py construct_vhost_scsi_controller naa.$disk.$vm_no
$rpc_py add_vhost_scsi_lun naa.$disk.$vm_no 0 Nvme0n1
$rpc_py get_vhost_controllers

vm_setup --disk-type=spdk_vhost_scsi --force=$vm_no --os=$vm_path --disks=$disk
vm_run $vm_no
vm_wait_for_boot 600 $vm_no

vm_check_scsi_location $vm_no
fio_disks+=" --vm=$vm_no$(printf ':/dev/%s' $SCSI_DISK)"
run_fio --fio-bin=$fio_bin --job-file="$fio_jobs" --out="$TEST_DIR/fio_results" $fio_disks

vm_shutdown_all
spdk_vhost_kill
kill_nvmf_tgt
