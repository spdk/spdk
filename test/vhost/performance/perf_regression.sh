BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../ && pwd)"
[[ -z "$WORKSPACE" ]] && WORKSPACE="$(cd $BASE_DIR/../../../../ && pwd)"
QUEUE_DEPTH=128
BLOCKSIZE=4096
RWMETHOD="read"
RUNTIME=30
RAMPTIME=10
FIOBIN=/home/sys_sgsw/fio_ubuntu

function usage() {
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for doing automated hotattach/hotdetach test"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "-h, --help                print help and exit"
	echo "    --queue-depth         specify queue_depth for fio tests"
	echo "    --blocksize           specify blocksize for fio tests"
	echo "    --rw                  specify comma separated rw methods for fio tests"
	echo "    --runtime             specify runtime for fio tests"
	echo "    --ramptime            specify ramptime for fio tests"
	echo "    --fio-bin             specify path to fio programm"
	exit 0
}

while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 ;;
			queue-depth=*) QUEUE_DEPTH="${OPTARG#*=}" ;;
			blocksize=*) BLOCKSIZE="${OPTARG#*=}" ;;
			rw=*) RWMETHOD="${OPTARG#*=}" ;;
			runtime=*) RUNTIME="${OPTARG#*=}" ;;
			ramptime=*) RAMPTIME="${OPTARG#*=}" ;;
			fio-bin=*) FIOBIN="${OPTARG#*=}" ;;
			*) usage $0 "Invalid argument '$OPTARG'" ;;
		esac
		;;
	h) usage $0 ;;
	x) set -x
		x="-x" ;;
	*) usage $0 "Invalid argument '$OPTARG'"
	esac
done
shift $(( OPTIND - 1 ))

. $BASE_DIR/../common/common.sh
vm_img="vhost_vm_image.qcow2"
DRIVES=2
VMS=$(($DRIVES))

# Prepare config files

cp vhost.conf.base $BASE_DIR/vhost.conf.in
echo "[Split]" >> $BASE_DIR/vhost.conf.in

DRIVES=6
for ((i=0 ; i<$DRIVES ; i++)); do
	echo "Split Nvme${i}n1 2" >> $BASE_DIR/vhost.conf.in
done
cat $BASE_DIR/vhost.conf.in

# cp $BASE_DIR/configs/smp2.config $BASE_DIR/../common/autotest.config
cat $BASE_DIR/../common/autotest.config

mkdir $WORKSPACE/fio_jobs || true


function create_fio_job() {
	local fio_job=$1
	local rw_method=$2
	rm $WORKSPACE/fio_jobs/$fio_job || true
	touch $WORKSPACE/fio_jobs/$fio_job
	cat << END_FIO_JOB >> $WORKSPACE/fio_jobs/$fio_job
[global]
blocksize=$BLOCKSIZE
iodepth=$QUEUE_DEPTH
ioengine=libaio
runtime=$RUNTIME
time_based=1
filename=
ramp_time=$RAMPTIME
group_reporting
thread
numjobs=1
direct=1
rw=$rw_method
[nvme-host]
END_FIO_JOB
}


# Randomize write pattern for write jobs
fio_job="$BLOCKSIZE"_"$RWMETHOD"_"$QUEUE_DEPTH".job
create_fio_job "$fio_job" "$RWMETHOD"

echo "egrep"
if [ "$RWMETHOD" == "write" ] || [ "$RWMETHOD" == "rw" ]; then
	echo -e "do_verify=0\nverify=meta\nverify_pattern=`hexdump -n 4 -e '\"0x%08X\"' /dev/urandom`" >> $WORKSPACE/fio_jobs/$fio_job
fi
sed -i "s#numjobs=1#numjobs=2#g" $WORKSPACE/fio_jobs/$fio_job
sed -i "s#nvme-host#$fio_job#g" $WORKSPACE/fio_jobs/$fio_job

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR
spdk_vhost_run --conf-path=$BASE_DIR
rm $BASE_DIR/vhost.conf.in
rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

used_vms=""
for ((i=0 ; i<$VMS ; i++))
do
	disk="Nvme$(($i/2))n1p$(($i%2))"
	os="/home/sys_sgsw/$vm_img"
	used_vms+=" $i"
	setup_cmd="vm_setup --disk-type=spdk_vhost_scsi --force=$i --os=$os --disks=$disk"
	$setup_cmd
	$rpc_py construct_vhost_scsi_controller naa.$disk.$i
	$rpc_py add_vhost_scsi_lun naa.$disk.$i 0 $disk
done
# Run VMs
vm_run $used_vms
vm_wait_for_boot 600 $used_vms

# Check if all VM have disk in tha same location
DISK=""

fio_disks=""
for vm_num in $used_vms; do
	vm_dir=$VM_BASE_DIR/$vm_num
	qemu_mask_param="VM_${vm_num}_qemu_mask"
	host_name="VM-$vm_num-${!qemu_mask_param}"
	notice "Setting up hostname: $host_name"
	vm_ssh $vm_num "hostname $host_name"
	vm_start_fio_server --fio-bin=$FIOBIN $readonly $vm_num
	vm_check_scsi_location $vm_num
	fio_disks+=" --vm=${vm_num}$(printf ':/dev/%s' $SCSI_DISK)"
done

run_fio --fio-bin=$FIOBIN --job-file="$WORKSPACE/fio_jobs/$fio_job" --out="$TEST_DIR/fio_results" $fio_disks

vm_shutdown_all
spdk_vhost_kill
