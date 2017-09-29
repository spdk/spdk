#!/usr/bin/env bash
set -ex
BASE_DIR=$(readlink -f $(dirname $0))
COMMON_DIR=$(readlink -f $BASE_DIR/../common)
TEST_DIR=$(readlink -f $BASE_DIR/../../../..)
ROOT_DIR=~

PMEM_SIZE=128
PMEM_BLOCK_SIZE=512
LOOP_RANGE=10

rpc_py="python $TEST_DIR/spdk/scripts/rpc.py"
RPC_PORT=5260

$COMMON_DIR/common.sh

os=$ROOT_DIR/vhost_vm_image.qcow2
work_dir=$TEST_DIR
fio_bin=$ROOT_DIR/fio_ubuntu
ctrl_type=spdk_vhost_blk

function fio_jobs()
{
    echo "[global]"
    echo "blocksize_range=4k-512k"
    echo "iodepth=512"
    echo "iodepth_batch=128"
    echo "iodepth_low=256"
    echo "ioengine=libaio"
    echo "size=1G"
    echo "io_size=4G"
    echo "filename=/dev/${disk[0]}"
    echo "group_reporting"
    echo "thread"
    echo "numjobs=1"
    echo "direct=1"
    echo "rw=randwrite"
    echo "do_verify=1"
    echo "verify=md5"
    echo "verify_backlog=1024"
    echo "[nvme-host]"
}

function clear_pmem_pool()
{
    for i in `seq 0 $LOOP_RANGE`; do
        if [ $ctrl_vhost_blk == "spdk_vhost_scsi" ]; then
            remove_vhost_scsi_dev naa.pmem$i.$i $(( $i % 8 ))
        fi
        $rpc_py remove_vhost_controller naa.pmem$i.$i
        $rpc_py delete_bdev pmem$i
        $rpc_py delete_pmem_pool /tmp/pool_file$i
    done
}

function prepare_fio()
{
    disk=""
    tmp_job=$BASE_DIR/fio.job.tmp
    run_fio="$fio_bin --eta=never "
    for vm_num in $1; do
        vm_dir=$VM_BASE_DIR/$vm_num
        if [ $ctrl_vhost_blk == "spdk_vhost_blk" ]; then
            vm_check_blk_location $vm_num
        else
            vm_check_scsi_location $vm_num
        fi
        disk=( $SCSI_DISK )
        fio_jobs >> $tmp_job
        vm_scp "$vm_num" $tmp_job 127.0.0.1:/root/fio_integrity_pmem.job
        run_fio+="--client=127.0.0.1,$(vm_fio_socket $vm_num) --remote-config /root/fio_integrity_pmem.job"
        rm $tmp_job
    done
}

function usage()
{
    [[ ! -z $2 ]] && ( echo "$2"; echo ""; )
    echo "Shortcut script for doing automated hotattach/hotdetach test"
    echo "Usage: $(basename $1) [OPTIONS]"
    echo
    echo "-h, --help                print help and exit"
    echo "    --ctrl-type=TYPE      Controller type to use for test:"
    echo "                          vhost_scsi - use spdk vhost scsi"
    echo "                          vhost_blk - use spdk vhost block"
    echo "    --fio-bin=FIO         Use specific fio binary (will be uploaded to VM)"
    echo "    --work-dir=WORK_DIR   Where to find build file. Must exist. [default: $ROOT_DIR]"
    echo "    --os=OS.qcow2        OS - VM os disk path (optional)"
    exit 0
}

while getopts 'xh-:' optchar; do
    case "$optchar" in
        -)
        case "$OPTARG" in
            help) usage $0 ;;
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

$COMMON_DIR/run_vhost.sh $x --work-dir=$work_dir --conf-dir=$BASE_DIR &
waitforlisten $! $RPC_PORT

trap "vm_shutdown_all; spdk_vhost_kill; rm -f /tmp/pool_file*; exit 1" SIGINT SIGTERM EXIT

for i in `seq 0 $LOOP_RANGE`; do
    $rpc_py create_pmem_pool /tmp/pool_file$i $PMEM_SIZE $PMEM_BLOCK_SIZE
    PMEM_BDEV="$($rpc_py construct_pmem_bdev /tmp/pool_file$i)"
    if [ $ctrl_vhost_blk == "spdk_vhost_blk" ]; then
        $rpc_py construct_vhost_blk_controller naa.$PMEM_BDEV.$i $PMEM_BDEV
    else
        $rpc_py construct_vhost_scsi_controller naa.$PMEM_BDEV.$i
        $rpc_py add_vhost_scsi_lun naa.$PMEM_BDEV.$i $(( $i % 8 )) $PMEM_BDEV
    fi
    $COMMON_DIR/vm_setup.sh $x --work-dir=$work_dir --test-type=$ctrl_type --os=$os --disk=$PMEM_BDEV -f $i
    if ! [ -d "${work_dir}/vms/${i}" ]; then
        echo "VM $i is not properly prepared"
        break
    fi
done

$COMMON_DIR/vm_run.sh $x --work-dir=$work_dir `seq 0 $LOOP_RANGE`
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
