#!/usr/bin/env bash
set -ex
BASE_DIR=$(readlink -f $(dirname $0))
ROOT_DIR=$(readlink -f $BASE_DIR/../../..)
TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"
COMMON_DIR=="$(cd $BASE_DIR/../common)"

PMEM_SIZE=128
PMEM_BLOCK_SIZE=512
LOOP_RANGE=0

rpc_py="python $ROOT_DIR/scripts/rpc.py"
RPC_PORT=5260
. $BASE_DIR/../common/common.sh

os=$TEST_DIR/vhost_vm_image.qcow2
work_dir=$TEST_DIR
fio_bin=$TEST_DIR/fio-2-99
test_type=spdk_vhost_blk
x=""

function fio_jobs ()
{
    echo "[global]"
    echo "ioengine=libaio"
    echo "size=120M"
    echo "io_size=10G"
    echo "filename=/dev/${disk[0]}"
    echo "numjobs=1"
    echo "bs=4k"
    echo "iodepth=128"
    echo "direct=1"
    echo "rw=randread"
    echo "group_reporting"
    echo "thread"
    echo "[nvme-host]"
}

function clear_pmem_pool()
{
    for i in `seq 0 $LOOP_RANGE`; do
        $rpc_py remove_vhost_controller naa.pmem$i.$i
        $rpc_py delete_bdev pmem$i
        $rpc_py delete_pmem_pool /tmp/pool_file$i
    done
}

function prepare_fio() {
    tmp_job=$BASE_DIR/fio.job.tmp
    run_fio="$fio_bin --eta=never "
    for vm_num in $1; do
        vm_dir=$VM_BASE_DIR/$vm_num
        vm_check_blk_location $vm_num
        disk=( $SCSI_DISK )
        fio_jobs>>$tmp_job
        vm_scp "$vm_num" $tmp_job 127.0.0.1:/root/default_integrity_disc.job
        run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/default_integrity_disc.job "
        rm $tmp_job
    done
}

function usage() {
    [[ ! -z $2 ]] && ( echo "$2"; echo ""; )
    echo "Shortcut script for doing automated hotattach/hotdetach test"
    echo "Usage: $(basename $1) [OPTIONS]"
    echo
    echo "-h, --help                print help and exit"
    echo "    --test-type=TYPE      Perform specified test:"
    echo "                          spdk_vhost_scsi - use spdk vhost scsi"
    echo "                          spdk_vhost_blk - use spdk vhost block"
    echo "-x                        set -x for script debug"
    echo "    --fio-bin=FIO         Use specific fio binary (will be uploaded to VM)"
    echo "    --work-dir=WORK_DIR   Where to find build file. Must exist. [default: $TEST_DIR]"
    echo "    --os= OS.qcow2        OS - VM os disk path (optional)"
    exit 0
}

while getopts 'xh-:' optchar; do
    case "$optchar" in
        -)
        case "$OPTARG" in
            help) usage $0 ;;
            work-dir=*) TEST_DIR="${OPTARG#*=}" ;;
            fio-bin=*) fio_bin="${OPTARG#*=}" ;;
            test-type=*) test_type="${OPTARG#*=}" ;;
            os=*) os="${OPTARG#*=}" ;;
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

$COMMON_DIR/run_vhost.sh $x --work-dir=$work_dir --conf-dir=$BASE_DIR &
waitforlisten $! $RPC_PORT

trap "vm_shutdown_all; spdk_vhost_kill; rm -f /tmp/pool_file*; exit 1" SIGINT SIGTERM EXIT

for i in `seq 0 $LOOP_RANGE`; do
    $rpc_py create_pmem_pool /tmp/pool_file$i $PMEM_SIZE $PMEM_BLOCK_SIZE
    PMEM_BDEV="$($rpc_py construct_pmem_bdev /tmp/pool_file$i)"
    $rpc_py construct_vhost_blk_controller naa.$PMEM_BDEV.$i $PMEM_BDEV
    $COMMON_DIR/vm_setup.sh $x --work-dir=$work_dir --test-type=$test_type --os=$os --disk=$PMEM_BDEV -f $i
    if [ -d "${work_dir}/vms/${i}" ]; then
        $COMMON_DIR/vm_run.sh $x --work-dir=$work_dir $i
    fi
done

vm_wait_for_boot 600 `seq 0 $LOOP_RANGE`
vms_prepare

for i in `seq 0 $LOOP_RANGE`; do
    prepare_fio $i
    $run_fio
done

trap - SIGINT SIGTERM EXIT

vm_shutdown_all

rm -f ./local-job*
clear_pmem_pool
spdk_vhost_kill
