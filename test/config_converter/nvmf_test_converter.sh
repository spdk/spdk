#!/usr/bin/env bash
CONVERTER_DIR=$(readlink -f $(dirname $0))
source $CONVERTER_DIR/common.sh
source $CONVERTER_DIR/../nvmf/common.sh

rdma_device_init
trap 'on_error_exit; revert_soft_roce' ERR

create_aio_bdevs
nvme_address=$(lspci -nn -D | grep -e '8086:5845' -e '8086:0953' | head -1 | awk '{print $1;}')
cp $CONVERTER_DIR/config_nvmf.ini $CONVERTER_DIR/config_tmp.ini
sed -i "s/0000:00:01.0/$nvme_address/g" $CONVERTER_DIR/config_tmp.ini
RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
sed -i "s/10.0.2.15/$NVMF_FIRST_TARGET_IP/g" $CONVERTER_DIR/config_tmp.ini
run_tests
killprocess $spdk_tgt_pid
spdk_tgt_pid=""
revert_soft_roce
test_cleanup
