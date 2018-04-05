#!/usr/bin/env bash
set -e
cd ../vhost/common
. ./common.sh
cd ../../json_config
BASE_DIR=$(readlink -f $(dirname $0))
. $BASE_DIR/../nvmf/common.sh
. $BASE_DIR/../common/autotest_common.sh
. $BASE_DIR/../iscsi_tgt/common.sh

vhost_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"
nvme_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/spdk.sock"
initiator_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir 1)/rpc.sock"
base_json_config=$BASE_DIR/base_config.json
last_json_config=$BASE_DIR/last_config.json
base_bdevs=$BASE_DIR/bdevs_base.txt
last_bdevs=$BASE_DIR/bdevs_last.txt

function run_vhost() {
    notice "==============="
    notice ""
    notice "running SPDK"
    notice ""
    spdk_vhost_run --conf-path=$BASE_DIR
    notice ""
}

function run_initiator() {
    cp $BASE_DIR/virtio.conf.base $BASE_DIR/vhost.conf.in
    spdk_vhost_run --conf-path=$BASE_DIR --vhost-num=1
}

# Add split section into vhost config
function gen_config() {
    cp $BASE_DIR/vhost.conf.base $BASE_DIR/vhost.conf.in
    cat << END_OF_CONFIG >> $BASE_DIR/vhost.conf.in
[Split]
  Split Nvme0n1 2
  Split Nvme1n1 2
END_OF_CONFIG
}

function test_json_config() {
    touch $base_bdevs
    touch $last_bdevs
    touch $base_json_config
    touch $last_json_config
    $rpc_py get_bdevs > $base_bdevs
    $rpc_py save_config -f $base_json_config
    echo "asdasd"
    cat $base_json_config
    $rpc_py clear_config
    echo "after"
    $rpc_py get_bdevs
    $rpc_py load_config --filename $base_json_config
    $rpc_py get_bdevs > $last_bdevs
    $rpc_py save_config -f $last_json_config
    diff $base_json_config $last_json_config || true
    diff $base_bdevs $last_bdevs || true
    rm $last_bdevs $base_bdevs || true
    rm $last_json_config $base_json_config || true
    $rpc_py clear_config
}

function upload_vhost() {
    $rpc_py construct_null_bdev Null0 32 512
    $rpc_py construct_malloc_bdev 128 512 --name Malloc0
    $rpc_py construct_malloc_bdev 64 4096 --name Malloc1
    $rpc_py construct_malloc_bdev 8 1024 --name Malloc2
    $rpc_py construct_error_bdev Malloc2
    $rpc_py construct_aio_bdev /root/sample_aio aio_disk 1024

}

function upload_nvmf() {
    bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
    bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"

    RDMA_IP_LIST=$(get_available_rdma_ips)
    NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
    if [ -z $NVMF_FIRST_TARGET_IP ]; then
        echo "no NIC for nvmf test"
        return
    fi

    $rpc_py construct_nvmf_subsystem nqn.2016-06.io.spdk:cnode1 '' '' -a -s SPDK00000000000001 -n "$bdevs"
    $rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t RDMA -a $NVMF_FIRST_TARGET_IP -s "$NVMF_PORT"
}

function upload_iscsi() {
    # iSCSI target configuration
    PORT=3260
    INITIATOR_TAG=1
    INITIATOR_NAME=ANY
    NETMASK=$INITIATOR_IP/32
    NVMF_PORT=4420
    PMEM_SIZE=32
    PMEM_BLOCK_SIZE=512
    TGT_NR=3
    PMEM_PER_TGT=1
    TARGET_IP=127.0.0.1
    INITIATOR_IP=127.0.0.1
    rdma_device_init
    RDMA_IP_LIST=$(get_available_rdma_ips)
    NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)

    $rpc_py add_portal_group 1 $TARGET_IP:$PORT
    if [ $NVMF_FIRST_TARGET_IP ]; then
        $rpc_py construct_nvme_bdev -b "Nvme0" -t "rdma" -f "ipv4" -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -n nqn.2016-06.io.spdk:cnode1
        $rpc_py construct_target_node Target1 Target1_alias 'Nvme0n1:0' '1:1' 64 -d
    fi
    for i in `seq 1 $TGT_NR`; do
        INITIATOR_TAG=$((i+1))
        $rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK

        luns=""
        for j in `seq 1 $PMEM_PER_TGT`; do
            $rpc_py create_pmem_pool /tmp/pool_file${i}_${j} $PMEM_SIZE $PMEM_BLOCK_SIZE
            bdevs_name="$($rpc_py construct_pmem_bdev -n pmem${i}_${j} /tmp/pool_file${i}_${j})"
            PMEM_BDEVS+="$bdevs_name "
            luns+="$bdevs_name:$((j-1)) "
        done
        $rpc_py construct_target_node Target$i Target${i}_alias "$luns" "1:$INITIATOR_TAG " 256 -d
    done
    rbd_setup
    $rpc_py construct_rbd_bdev $RBD_POOL $RBD_NAME 4096
}

function clean_upload_vhost() {
    rpc_py="$vhost_rpc_py"
}

function clean_upload_nvmf() {
    rpc_py="$nvme_rpc_py"
}

function clean_upload_iscsi() {
    rpc_py="$nvme_rpc_py"
    rbd_cleanup
    PMEM_PER_TGT=1
    TGT_NR=3

    for i in `seq 1 $TGT_NR`; do
        INITIATOR_TAG=$((i+1))
        for j in `seq 1 $PMEM_PER_TGT`; do
            $rpc_py delete_pmem_pool /tmp/pool_file${i}_${j}
        done
    done
}

function pre_initiator_config() {
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p0.0
    $rpc_py construct_vhost_scsi_controller naa.Nvme1n1p0.1
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0
    $rpc_py add_vhost_scsi_lun naa.Nvme1n1p0.1 0 Nvme1n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p1.0 Nvme0n1p1
    $rpc_py construct_vhost_blk_controller naa.Nvme1n1p1.1 Nvme1n1p1
}

function upload_initiator() {
    $rpc_py construct_virtio_user_scsi_bdev $(get_vhost_dir 0)/naa.Nvme0n1p0.0 Nvme0n1p0
    $rpc_py construct_virtio_user_blk_bdev $(get_vhost_dir 0)/naa.Nvme0n1p1.0 Nvme0n1p1
}

function test_vhost() {
    gen_config
    run_vhost

    rpc_py="$vhost_rpc_py"
    upload_vhost
    test_json_config
    pre_initiator_config
    run_initiator
    rpc_py="$initiator_rpc_py"
    upload_initiator
    test_json_config
    clean_upload_vhost
    for vhost_num in $(spdk_vhost_list_all); do
        spdk_vhost_kill $vhost_num
    done
}

function test_nvmf() {
    MALLOC_BDEV_SIZE=64
    MALLOC_BLOCK_SIZE=512

    $BASE_DIR/../../app/nvmf_tgt/nvmf_tgt -c $BASE_DIR/../nvmf/nvmf.conf &
    nvmfpid=$!
    waitforlisten $nvmfpid

    rpc_py="$nvme_rpc_py"
    upload_nvmf
    test_json_config
    clean_upload_nvmf

    #nvmfcleanup
    killprocess $nvmfpid
}

function test_iscsi() {
    rootdir=$(readlink -f $BASE_DIR/../..)
    $BASE_DIR/../../app/iscsi_tgt/iscsi_tgt -i 0 -c $BASE_DIR/../iscsi_tgt/calsoft/iscsi.conf -m 0x1 &
    iscsipid=$!
    waitforlisten $iscsipid

    rpc_py="$nvme_rpc_py"
    upload_iscsi
    test_json_config
    clean_upload_iscsi

    killprocess $iscsipid
}

function on_error_exit() {
    set +e
    echo "Error on $1 - $2"
    rbd_cleanup || true
    killprocess $iscsipid || true
    killprocess $nvmfpid || true
    for vhost_num in $(spdk_vhost_list_all); do
        spdk_vhost_kill $vhost_num
    done
    rm $last_bdevs $base_bdevs || true
    rm $last_json_config $base_json_config || true
    rm $BASE_DIR/vhost.conf.in || true
    rm /tmp/pool_file* || true
    print_backtrace
    exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
modprobe nbd

test_vhost
test_nvmf
test_iscsi

rmmod nbd
