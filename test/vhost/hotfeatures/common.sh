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
force_build=false
single_vm=0
nvme_disk="Nvme0"
nvme_disk1="Nvme1"
test_case="all"
disk_split=""
used_vms=""
vms=()
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
    echo "    --test-case           choose test case or run all tests e.g. --test-case=all or --test-case=test_case1"
    echo "    --nvme-disk           fist name NVME disk, which is used to tests"
    echo "    --nvme-disk1          second name NVME disk, which is used to tests"
    echo "    --single-vm           vm number used for test with single vm"
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
            test-case=*) test_case="${OPTARG#*=}" ;;
            nvme-disk=*) nvme_disk="${OPTARG#*=}" ;;
            nvme-disk1=*) nvme_disk1="${OPTARG#*=}" ;;
            single-vm=*) single_vm="${OPTARG#*=}" ;;
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
tmp_job=$BASE_DIR/fio_jobs/fio.job.tmp
. $BASE_DIR/../common/common.sh

rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s 127.0.0.1 "

function back_configuration() {
    rm $BASE_DIR/vhost.conf.in
    echo "$1">/sys/bus/pci/drivers/uio_pci_generic/bind
}

function config_base() {
    cat $BASE_DIR/vhost.conf.base>$BASE_DIR/vhost.conf.in
    $SPDK_BUILD_DIR/scripts/gen_nvme.sh>$BASE_DIR/vhost.conf.in
    echo "HotplugEnable Yes">$BASE_DIR/vhost.conf.in
    echo "[Split]">$BASE_DIR/vhost.conf.in
    echo "  Split Nvme0n1 2">$BASE_DIR/vhost.conf.in
    echo "  Split Nvme1n1 2">$BASE_DIR/vhost.conf.in
}

function check_fio_retcode() {
    fio_retcode=$3
    echo $1
    retcode_expected=$2
    if test $retcode_expected == 0; then
        if [ "$fio_retcode" != 0 ]; then
            echo "    Fio test ended with error."
            vm_shutdown_all
            spdk_vhost_kill
            exit 1
        else
            echo "    Fio test ended with success."
        fi
    else
        if [ "$fio_retcode" != 0 ]; then
            echo "    Fio test ended with expected error."
        else
            echo "    Fio test ended with unexpected success."
            vm_shutdown_all
            spdk_vhost_kill
            exit 1
        fi
    fi
}

function get_disk() {
    vm_check_scsi_location $1
    disk_array=( $SCSI_DISK )
    NVME_DISK=${disk_array[$1]}
}

function get_nvme_pci_addr() {
    chmod 755 $1
    [[ $(  grep $2 $1 ) =~ ([0-9a-fA-F]{4}(:[0-9a-fA-F]{2}){2}.[0-9a-fA-F]) ]]
    echo ${BASH_REMATCH[1]}
}

function run_vhost() {
    echo "==============="
    echo ""
    echo "INFO: running SPDK"
    echo ""
    $BASE_DIR/../common/run_vhost.sh $x --work-dir=$TEST_DIR --conf-dir=$BASE_DIR
    echo
}

function vms_setup() {
    for vm_conf in ${vms[@]}; do
        IFS=',' read -ra conf <<< "$vm_conf"
        setup_cmd="$BASE_DIR/../common/vm_setup.sh $x --work-dir=$TEST_DIR --test-type=$test_type"
        if [[ x"${conf[0]}" == x"" ]] || ! assert_number ${conf[0]}; then
            echo "ERROR: invalid VM configuration syntax $vm_conf"
            exit 1;
        fi

        # Sanity check if VM is not defined twice
        for vm_num in $used_vms; do
            if [[ $vm_num -eq ${conf[0]} ]]; then
                echo "ERROR: VM$vm_num defined more than twice ( $(printf "'%s' " "${vms[@]}"))!"
                exit 1
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
    $BASE_DIR/../common/vm_run.sh $x --work-dir=$TEST_DIR $1
    vm_wait_for_boot 600 $1
}

function vms_prepare() {
    for vm_num in $1; do
        vm_dir=$VM_BASE_DIR/$vm_num

        qemu_mask_param="VM_${vm_num}_qemu_mask"

        host_name="VM-${vm_num}-${!qemu_mask_param}"
        echo "INFO: Setting up hostname: $host_name"
        vm_ssh $vm_num "hostname $host_name"
        vm_start_fio_server --fio-bin=$fio_bin $readonly $vm_num
    done
}

function vms_reboot() {
    vms_reboot_all $1
    vms_prepare $1
}

function vms_reboot_all() {
    echo "Rebooting all vms "
    for vm_num in $1; do
        vm_ssh $vm_num "reboot" || true
    done

    vm_wait_for_boot 600 $1
}

function vms_start() {
    vms_setup_and_run $1
    vms_prepare $1
}

function unbind_bdev() {
    (sleep 5; echo "$1">/sys/bus/pci/devices/$bdf/driver/unbind) &
}

function prepare_fio() {
    run_fio="$fio_bin --eta=never "
    for vm_num in $1; do
        cp $fio_job $tmp_job
        vm_dir=$VM_BASE_DIR/$vm_num
        if [[ $test_type == "spdk_vhost_scsi" ]]; then
            vm_check_scsi_location $vm_num
        else
            vm_check_blk_location $vm_num
        fi
        for disk in $SCSI_DISK; do
            echo "[nvme-host$disk]"
            echo "filename=/dev/$disk"
            echo "time_based=20"
        done >> $tmp_job
        vm_scp "$vm_num" $tmp_job 127.0.0.1:/root/default_integrity_discs.job
        run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity_discs.job "
        rm $tmp_job
    done
}

function print_test_fio_header() {
    echo "==============="
    echo ""
    echo "INFO: Testing..."

    echo "INFO: Running fio jobs ..."
    if [ $# -gt 0 ]; then
        echo $1
    fi
}
