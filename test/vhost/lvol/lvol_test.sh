#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"
[[ -z "$COMMON_DIR" ]] && COMMON_DIR="$(cd $BASE_DIR/../common && pwd)"

. $COMMON_DIR/common.sh

rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py "

vm_count=1
max_disks=""
ctrl_type="vhost_scsi"
use_fs=false
nested_lvol=false
distribute_cores=false
base_bdev_size=10000
nest_bdev_size=""

function usage()
{
    [[ ! -z $2 ]] && ( echo "$2"; echo ""; )
    echo "Shortcut script for doing automated test"
    echo "Usage: $(basename $1) [OPTIONS]"
    echo
    echo "-h, --help                Print help and exit"
    echo "    --fio-bin=PATH        Path to FIO binary.;"
    echo "    --vm-count=INT        Virtual machines to use in test;"
    echo "                          Each VM will get one lvol bdev on each NVMe."
    echo "                          Default: 1"
    echo "    --max-disks=INT       Maximum number of NVMe drives to use in test."
    echo "                          Default: will use all available NVMes."
    echo "    --ctrl-type=TYPE      Controller type to use for test:"
    echo "                          vhost_scsi - use spdk vhost scsi"
    echo "                          vhost_blk - use spdk vhost block"
    echo "    --nested-lvol         If enabled will create additional lvol bdev"
    echo "                          on each NVMe for use as base device for next"
    echo "                          lvol store and lvol bdevs."
    echo "                          (NVMe->lvol_store->lvol_bdev->lvol_store->lvol_bdev)"
    echo "                          Default: False"
    echo "-x                        set -x for script debug"
    echo "    --distribute-cores    Use custom config file and run vhost controllers"
    echo "                          on different CPU cores instead of single core."
    echo "                          Default: False"
    exit 0
}

while getopts 'xh-:' optchar; do
    case "$optchar" in
        -)
        case "$OPTARG" in
            help) usage $0 ;;
            fio-bin=*) fio_bin="${OPTARG#*=}" ;;
            vm-count=*) vm_count="${OPTARG#*=}" ;;
            max-disks=*) max_disks="${OPTARG#*=}" ;;
            ctrl-type=*) ctrl_type="${OPTARG#*=}" ;;
            nested-lvol) nested_lvol=true ;;
            distribute-cores) distribute_cores=true ;;
            *) usage $0 "Invalid argument '$OPTARG'" ;;
        esac
        ;;
    h) usage $0 ;;
    x) set -x
        x="-x" ;;
    *) usage $0 "Invalid argument '$OPTARG'"
    esac
done

echo "INFO: Get NVMe disks:"
nvmes=($(lspci -mm -n | grep 0108 | tr -d '"' | awk -F " " '{print "0000:"$1}'))

if [[ -z $max_disks ]]; then
    max_disks=${#nvmes[@]}
fi

if [[ ${#nvmes[@]} -lt max_disks ]]; then
    echo -e "ERROR: Number of NVMe drives (${#nvmes[@]})\n\
is lower than number of requested disks for test ($max_disks)"
    exit 1
fi

if $distribute_cores; then
    cp $COMMON_DIR/autotest.config $COMMON_DIR/autotest.config.bak
    cp $BASE_DIR/autotest.config $COMMON_DIR/autotest.config
    . $COMMON_DIR/common.sh
fi

function restore_acfg()
{
    if $distribute_cores; then
        mv $COMMON_DIR/autotest.config.bak $COMMON_DIR/autotest.config
    fi
}

trap 'restore_acfg; error_exit "${FUNCNAME}" "${LINENO}"' ERR

vm_kill_all

echo "==============="
echo ""
echo "INFO: checking spdk"
echo ""
if [[ ! -x $SPDK_BUILD_DIR/app/vhost/vhost ]]; then
    echo "ERROR: SPDK Vhost is not present - please build it."
    exit 1
fi

echo "INFO: running SPDK"
echo ""
$COMMON_DIR/run_vhost.sh $x --work-dir=$TEST_DIR --conf-dir=$BASE_DIR
echo ""

lvol_stores=()
lvol_bdevs=()
nest_lvol_stores=()
nest_lvol_bdevs=()
used_vms=""

# On each NVMe create one lvol store
for (( i=0; i<$max_disks; i++ ));do
    echo "INFO: Creating lvol store on device Nvme${i}n1"
    ls_guid=$($rpc_py construct_lvol_store Nvme${i}n1 lvs_$i)
    lvol_stores+=("$ls_guid")
done

# Create lvol bdev for nested lvol stores if needed
if $nested_lvol; then
    for lvol_store in "${lvol_stores[@]}"; do

        echo "INFO: Creating lvol bdev on lvol store $lvol_store"
        lb_name=$($rpc_py construct_lvol_bdev -u $lvol_store lbd_nest 16000)
        lvol_bdevs+=("$lb_name")

        echo "INFO: Creating nested lvol store on lvol bdev: $lb_guid"
        ls_guid=$($rpc_py construct_lvol_store $lb_guid lvs_n_$i)
        nest_lvol_stores+=("$ls_guid")
    done
fi

# For each VM create one lvol bdev on each 'normal' and nested lvol store
for (( i=0; i<$vm_count; i++)); do
    bdevs=()
    echo "INFO: Creating lvol bdevs for VM $i"
    for lvol_store in "${lvol_stores[@]}"; do
        lb_name=$($rpc_py construct_lvol_bdev -u $lvol_store lbd_$i 10000)
        lvol_bdevs+=("$lb_name")
        bdevs+=("$lb_name")
    done

    if $nested_lvol; then
        echo "INFO: Creating nested lvol bdevs for VM $i"
        for lvol_store in "${nest_lvol_stores[@]}"; do
            lb_guid=$($rpc_py construct_lvol_bdev -u $lvol_store lbd_nest_$i 2000)
            nest_lvol_bdevs+=("$lb_guid")
            bdevs+=("$lb_guid")
        done
    fi

    setup_cmd="$COMMON_DIR/vm_setup.sh $x --work-dir=$TEST_DIR"
    if [[ "$ctrl_type" == "vhost_scsi" ]]; then
        setup_cmd+=" --test-type=spdk_vhost_scsi"
    elif [[ "$ctrl_type" == "vhost_blk" ]]; then
        setup_cmd+=" --test-type=spdk_vhost_blk"
    fi
    setup_cmd+=" -f $i"
    setup_cmd+=" --os=/home/sys_sgsw/vhost_vm_image.qcow2"

    # Create single SCSI controller or multiple BLK controllers for this VM
    if $distribute_cores; then
        mask="VM_${i}_qemu_mask"
        mask_arg="--cpumask ${!mask}"
    fi

    if [[ "$ctrl_type" == "vhost_scsi" ]]; then
        $rpc_py construct_vhost_scsi_controller naa.0.$i $mask_arg
        for (( j=0; j<${#bdevs[@]}; j++)); do
            $rpc_py add_vhost_scsi_lun naa.0.$i $j ${bdevs[$j]}
        done
        setup_cmd+=" --disk=0"
    elif [[ "$ctrl_type" == "vhost_blk" ]]; then
        disk=""
        for (( j=0; j<${#bdevs[@]}; j++)); do
            $rpc_py construct_vhost_blk_controller naa.$j.$i ${bdevs[$j]} $mask_arg
            disk+="${j}_size_1500M:"
        done
        disk="${disk::-1}"
        setup_cmd+=" --disk=$disk"
    fi

    $setup_cmd
    used_vms+=" $i"
done

$rpc_py get_lvol_stores
$rpc_py get_bdevs
$rpc_py get_vhost_controllers
$rpc_py get_luns

# Run VMs
$COMMON_DIR/vm_run.sh $x --work-dir=$TEST_DIR $used_vms
vm_wait_for_boot 600 $used_vms

# Get disk names from VMs and run FIO traffic
run_fio="python $COMMON_DIR/run_fio.py --fio-bin=$fio_bin"
run_fio+=" --job-file=$COMMON_DIR/fio_jobs/default_integrity.job"
run_fio+=" --out=$TEST_DIR "

for vm_num in $used_vms; do
    vm_dir=$VM_BASE_DIR/$vm_num
    qemu_mask_param="VM_${vm_num}_qemu_mask"

    host_name="VM-$vm_num-${!qemu_mask_param}"
    vm_ssh $vm_num "hostname $host_name"
    vm_start_fio_server --fio-bin=$fio_bin $vm_num

    if [[ "$ctrl_type" == "vhost_scsi" ]]; then
        vm_check_scsi_location $vm_num
    elif [[ "$ctrl_type" == "vhost_blk" ]]; then
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

echo "INFO: Shutting down virtual machines..."
vm_shutdown_all
sleep 2

echo "INFO: Cleaning up vhost - remove LUNs, controllers, lvol bdevs and lvol stores"
if [[ "$ctrl_type" == "vhost_scsi" ]]; then
    for (( i=0; i<$vm_count; i++)); do
        echo "INFO: Removing devices from vhost SCSI controller naa.0.$i"
        for (( j=0; j<${#bdevs[@]}; j++)); do
            $rpc_py remove_vhost_scsi_dev naa.0.$i $j
            echo -e "\tINFO: Removed device $j"
        done
        echo "Removing vhost SCSI controller naa.0.$i"
        $rpc_py remove_vhost_controller naa.0.$i
    done
elif [[ "$ctrl_type" == "vhost_blk" ]]; then
    for (( i=0; i<$vm_count; i++)); do
        for (( j=0; j<${#bdevs[@]}; j++)); do
            echo "INFO: Removing vhost BLK controller naa.$j.$i"
            $rpc_py remove_vhost_controller naa.$j.$i
            echo -e "\tINFO: Removed naa.$j.$i"
        done
    done
fi

echo "INFO: Removing nested lvol bdevs"
for lvol_bdev in "${nest_lvol_bdevs[@]}"; do
    $rpc_py delete_bdev $lvol_bdev
    echo -e "\tINFO: nested lvol bdev $lvol_bdev removed"
done

echo "INFO: Removing nested lvol stores"
for lvol_store in "${nest_lvol_stores[@]}"; do
    $rpc_py destroy_lvol_store -u $lvol_store
    echo -e "\tINFO: nested lvol store $lvol_store removed"
done

echo "INFO: Removing lvol bdevs"
for lvol_bdev in "${lvol_bdevs[@]}"; do
    $rpc_py delete_bdev $lvol_bdev
    echo -e "\tINFO: lvol bdev $lvol_bdev removed"
done

echo "INFO: Removing lvol stores"
for lvol_store in "${lvol_stores[@]}"; do
    $rpc_py destroy_lvol_store -u $lvol_store
    echo -e "\tINFO: lvol store $lvol_store removed"
done

$rpc_py get_lvol_stores
$rpc_py get_bdevs
$rpc_py get_vhost_controllers
$rpc_py get_luns

echo "INFO: Shutting down SPDK vhost app..."
spdk_vhost_kill
restore_acfg
