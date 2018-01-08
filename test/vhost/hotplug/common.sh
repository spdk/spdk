#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

dry_run=false
no_shutdown=false
fio_bin="fio"
fio_jobs="$BASE_DIR/fio_jobs/"
test_type=spdk_vhost_scsi
reuse_vms=false
vms=()
used_vms=""
disk_split=""
x=""


function usage() {
    [[ ! -z $2 ]] && ( echo "$2"; echo ""; )
    echo "Shortcut script for doing automated hotattach/hotdetach test"
    echo "Usage: $(basename $1) [OPTIONS]"
    echo
    echo "-h, --help                print help and exit"
    echo "    --test-type=TYPE      Perform specified test:"
    echo "                          virtio - test host virtio-scsi-pci using file as disk image"
    echo "                          kernel_vhost - use kernel driver vhost-scsi"
    echo "                          spdk_vhost_scsi - use spdk vhost scsi"
    echo "                          spdk_vhost_blk - use spdk vhost block"
    echo "-x                        set -x for script debug"
    echo "    --fio-bin=FIO         Use specific fio binary (will be uploaded to VM)"
    echo "    --fio-jobs=           Fio configs to use for tests. Can point to a directory or"
    echo "    --work-dir=WORK_DIR   Where to find build file. Must exist. [default: $TEST_DIR]"
    echo "    --vm=NUM[,OS][,DISKS] VM configuration. This parameter might be used more than once:"
    echo "                          NUM - VM number (mandatory)"
    echo "                          OS - VM os disk path (optional)"
    echo "                          DISKS - VM os test disks/devices path (virtio - optional, kernel_vhost - mandatory)"
    echo "                          If test-type=spdk_vhost_blk then each disk can have additional size parameter, e.g."
    echo "                          --vm=X,os.qcow,DISK_size_35G; unit can be M or G; default - 20G"
    exit 0
}

while getopts 'xh-:' optchar; do
    case "$optchar" in
        -)
        case "$OPTARG" in
            help) usage $0 ;;
            work-dir=*) TEST_DIR="${OPTARG#*=}" ;;
            fio-bin=*) fio_bin="${OPTARG#*=}" ;;
            fio-jobs=*) fio_jobs="${OPTARG#*=}" ;;
            test-type=*) test_type="${OPTARG#*=}" ;;
            vm=*) vms+=("${OPTARG#*=}") ;;
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

fio_job=$BASE_DIR/fio_jobs/default_integrity.job
tmp_attach_job=$BASE_DIR/fio_jobs/fio_attach.job.tmp
tmp_detach_job=$BASE_DIR/fio_jobs/fio_detach.job.tmp
. $BASE_DIR/../common/common.sh

rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py "

function print_test_fio_header() {
    notice "==============="
    notice ""
    notice "Testing..."

    notice "Running fio jobs ..."
    if [ $# -gt 0 ]; then
        echo $1
    fi
}

function run_vhost() {
    notice "==============="
    notice ""
    notice "running SPDK"
    notice ""
    $BASE_DIR/../common/run_vhost.sh $x --work-dir=$TEST_DIR --conf-dir=$BASE_DIR
    notice ""
}

function vms_setup() {
    for vm_conf in ${vms[@]}; do
        IFS=',' read -ra conf <<< "$vm_conf"
        setup_cmd="$BASE_DIR/../common/vm_setup.sh $x --work-dir=$TEST_DIR --test-type=$test_type"
        if [[ x"${conf[0]}" == x"" ]] || ! assert_number ${conf[0]}; then
            fail "invalid VM configuration syntax $vm_conf"
        fi

        # Sanity check if VM is not defined twice
        for vm_num in $used_vms; do
            if [[ $vm_num -eq ${conf[0]} ]]; then
                fail "VM$vm_num defined more than twice ( $(printf "'%s' " "${vms[@]}"))!"
            fi
        done

        setup_cmd+=" -f ${conf[0]}"
        used_vms+=" ${conf[0]}"
        [[ x"${conf[1]}" != x"" ]] && setup_cmd+=" --os=${conf[1]}"
        [[ x"${conf[2]}" != x"" ]] && setup_cmd+=" --disk=${conf[2]}"

        $setup_cmd
    done
}

function vms_setup_and_run() {
    vms_setup
    # Run everything
    $BASE_DIR/../common/vm_run.sh $x --work-dir=$TEST_DIR $used_vms
    vm_wait_for_boot 600 $used_vms
}

function vms_prepare() {
    for vm_num in $1; do
        vm_dir=$VM_BASE_DIR/$vm_num

        qemu_mask_param="VM_${vm_num}_qemu_mask"

        host_name="VM-${vm_num}-${!qemu_mask_param}"
        notice "Setting up hostname: $host_name"
        vm_ssh $vm_num "hostname $host_name"
        vm_start_fio_server --fio-bin=$fio_bin $readonly $vm_num
    done
}

function vms_reboot_all() {
    notice "Rebooting all vms "
    for vm_num in $1; do
        vm_ssh $vm_num "reboot" || true
    done

    vm_wait_for_boot 600 $1
}

function check_fio_retcode() {
    fio_retcode=$3
    echo $1
    retcode_expected=$2
    if [ $retcode_expected == 0 ]; then
        if [ $fio_retcode != 0 ]; then
            warning "    Fio test ended with error."
            vm_shutdown_all
            spdk_vhost_kill
            exit 1
        else
            notice "    Fio test ended with success."
        fi
    else
        if [ $fio_retcode != 0 ]; then
            notice "    Fio test ended with expected error."
        else
            warning "    Fio test ended with unexpected success."
            vm_shutdown_all
            spdk_vhost_kill
            exit 1
        fi
    fi
}

function reboot_all_and_prepare() {
    vms_reboot_all $1
    vms_prepare $1
}
