#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))

. $BASE_DIR/../common/common.sh

vhost_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"
nvme_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/spdk.sock"

function run_vhost() {
    notice "==============="
    notice ""
    notice "running SPDK"
    notice ""
    spdk_vhost_run --conf-path=$BASE_DIR
    notice ""
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

function test_rpc_subsystem() {
    $rpc_py get_subsystems
    $rpc_py get_subsystem_config copy
    $rpc_py get_subsystem_config interface
    $rpc_py get_subsystem_config net_framework
    $rpc_py get_subsystem_config bdev
    $rpc_py get_subsystem_config nbd
    $rpc_py get_subsystem_config scsi
    $rpc_py get_subsystem_config vhost
}

function test_json_config() {
    $rpc_py save_config
    $rpc_py save_config -f $BASE_DIR/sample.json
    $rpc_py load_config --filename $BASE_DIR/sample.json
    $rpc_py save_config -f $BASE_DIR/sample_2.json
    diff $BASE_DIR/sample.json $BASE_DIR/sample_2.json
    rm $BASE_DIR/sample.json $BASE_DIR/sample_2.json || true
}

function upload_vhost() {
    $rpc_py get_bdevs
    $rpc_py construct_vhost_scsi_controller naa.Nvme0n1p0.0
    $rpc_py construct_vhost_scsi_controller naa.Nvme1n1p0.1
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0
    $rpc_py add_vhost_scsi_lun naa.Nvme1n1p0.1 0 Nvme1n1p0
    $rpc_py construct_vhost_blk_controller naa.Nvme0n1p1.0 Nvme0n1p1
    $rpc_py construct_vhost_blk_controller naa.Nvme1n1p1.1 Nvme1n1p1

    $rpc_py construct_malloc_bdev 128 512 --name Malloc0
    $rpc_py construct_malloc_bdev 64 4096 --name Malloc1
    $rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 1 Malloc0
    $rpc_py construct_vhost_blk_controller naa.Malloc1.0 Malloc1

    $rpc_py construct_lvol_store Malloc0 lvs_test -c 1048576
    $rpc_py construct_lvol_bdev -l lvs_test lvol0 32
    $rpc_py start_nbd_disk lvs_test/lvol0 /dev/nbd0
    #$rpc_py snapshot_lvol_bdev lvol0 snap0
    #$rpc_py clone_lvol_bdev snap0 clone0
}

function upload_nvmet() {
    bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
    bdevs="$bdevs $($rpc_py construct_malloc_bdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE)"
    echo "BDEVS: $bdevs"

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
    INITIATOR_TAG=2
    INITIATOR_NAME=ANY
    NETMASK=$INITIATOR_IP/32
    MALLOC_BDEV_SIZE=64
    MALLOC_BLOCK_SIZE=512
    ISCSI_PORT=3260
    NVMF_PORT=4420
    PMEM_SIZE=128
    PMEM_BLOCK_SIZE=512
    TGT_NR=10
    PMEM_PER_TGT=1

    $rpc_py add_portal_group 1 $TARGET_IP:$PORT
    $rpc_py add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
    $rpc_py construct_malloc_bdev -b MyBdev $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE
    # "MyBdev:0" ==> use MyBdev blockdev for LUN0
    # "1:2" ==> map PortalGroup1 to InitiatorGroup2
    # "64" ==> iSCSI queue depth 64
    # "0 0 0 1" ==> enable CHAP authentication using auth group 1
    $rpc_py construct_target_node Target3 Target3_alias 'MyBdev:0' '1:2' 64 -g 1
    $rpc_py add_portal_group 1 $TARGET_IP:$ISCSI_PORT
    $rpc_py add_initiator_group 1 ANY $INITIATOR_IP/32
    $rpc_py construct_nvme_bdev -b "Nvme0" -t "rdma" -f "ipv4" -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT -n nqn.2016-06.io.spdk:cnode1
    $rpc_py construct_target_node Target1 Target1_alias 'Nvme0n1:0' '1:1' 64 -d
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
    $rpc_py construct_rbd_bdev $RBD_POOL $RBD_NAME 4096
    $rpc_py construct_target_node Target3 Target3_alias 'Ceph0:0' '1:2' 64 -d
}

function test_vhost() {
    run_vhost

    rpc_py="$vhost_rpc_py"
    upload_vhost
    $rpc_py save_config
    $rpc_py clear_config
    $rpc_py get_bdevs
    #test_rpc_subsystem
    test_json_config

    spdk_vhost_kill
}

function test_nvmf() {
    . $BASE_DIR/../../nvmf/common.sh
    #modprobe -v nvme-rdma
    MALLOC_BDEV_SIZE=64
    MALLOC_BLOCK_SIZE=512

    $BASE_DIR/../../../app/nvmf_tgt/nvmf_tgt -c $BASE_DIR/../../nvmf/nvmf.conf &
    nvmfpid=$!
    trap "killprocess $nvmfpid; exit 1" SIGINT SIGTERM EXIT

    waitforlisten $nvmfpid

    rpc_py="$nvme_rpc_py"
    upload_nvmet
    $rpc_py save_config
    $rpc_py clear_config
    test_json_config

    #nvmfcleanup
    killprocess $nvmfpid
}

function test_iscsi() {
    . $BASE_DIR/../../iscsi_tgt/common.sh
    $BASE_DIR/../../../app/iscsi_tgt/iscsi_tgt -i 0 -c $BASE_DIR/../../iscsi_tgt/calsoft/iscsi.conf -m 0x1 &
    iscsipid=$!
    echo "Process pid: $iscsipid"

    trap "killprocess $iscsipid; exit 1 " SIGINT SIGTERM EXIT
    waitforlisten $iscsipid
    rpc_py="$nvme_rpc_py"
    upload_iscsi
    $rpc_py save_config
    $rpc_py clear_config
    test_json_config

    killprocess $iscsipid
}

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR
gen_config
modprobe nbd

#test_vhost
#test_nvmf
test_iscsi

rmmod nbd
