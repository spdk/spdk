#!/usr/bin/env bash
set -xe

MATCH_FILE="spdkcli_nvmf.test"
SPDKCLI_BRANCH="/nvmf"
testdir=$(readlink -f $(dirname $0))
. $testdir/common.sh
. $testdir/../nvmf/common.sh

timing_enter spdkcli_nvmf
trap 'on_error_exit; revert_soft_roce' ERR
rdma_device_init

timing_enter run_spdk_tgt
run_spdk_tgt
timing_exit run_spdk_tgt

RDMA_IP_LIST=$(get_available_rdma_ips)
NVMF_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)

timing_enter spdkcli_create_nvmf_config
$spdkcli_job "/bdevs/malloc create 32 512 Malloc1" "Malloc1" True
$spdkcli_job "/bdevs/malloc create 32 512 Malloc2" "Malloc2" True
$spdkcli_job "/bdevs/malloc create 32 512 Malloc3" "Malloc3" True
$spdkcli_job "/bdevs/malloc create 32 512 Malloc4" "Malloc4" True
$spdkcli_job "/bdevs/malloc create 32 512 Malloc5" "Malloc5" True
$spdkcli_job "/bdevs/malloc create 32 512 Malloc6" "Malloc6" True
$spdkcli_job "/nvmf/subsystem create nqn.2014-08.org.spdk:cnode1 N37SXV509SRW\
  max_namespaces=4 allow_any_host=True" "nqn.2014-08.org.spdk:cnode1" True
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode1/namespaces create Malloc3 1" "Malloc3" True
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode1/namespaces create Malloc4 2" "Malloc4" True
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode1/listen_addresses create \
 RDMA $NVMF_TARGET_IP 4260 IPv4" "$NVMF_TARGET_IP:4260" True
$spdkcli_job "/nvmf/subsystem create nqn.2014-08.org.spdk:cnode2 N37SXV509SRD\
 max_namespaces=2 allow_any_host=True" "nqn.2014-08.org.spdk:cnode2" True
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode2/namespaces create Malloc2" "Malloc2" True
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode2/listen_addresses create \
 RDMA $NVMF_TARGET_IP 4260 IPv4" "$NVMF_TARGET_IP:4260" True
$spdkcli_job "/nvmf/subsystem create nqn.2014-08.org.spdk:cnode3 N37SXV509SRR\
 max_namespaces=2 allow_any_host=True" "nqn.2014-08.org.spdk:cnode2" True
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode3/namespaces create Malloc1" "Malloc1" True
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode3/listen_addresses create \
 RDMA $NVMF_TARGET_IP 4260 IPv4" "$NVMF_TARGET_IP:4260" True
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode3/listen_addresses create \
 RDMA $NVMF_TARGET_IP 4261 IPv4" "$NVMF_TARGET_IP:4261" True
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode3/hosts create \
 nqn.2014-08.org.spdk:cnode1" "nqn.2014-08.org.spdk:cnode1" True
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode3/hosts create \
 nqn.2014-08.org.spdk:cnode2" "nqn.2014-08.org.spdk:cnode2" True
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode1 allow_any_host True" "Allow any host"
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode1 allow_any_host False" "Allow any host" True
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode1/listen_addresses create RDMA $NVMF_TARGET_IP 4261 IPv4" "$NVMF_TARGET_IP:4261" True
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode1/listen_addresses create RDMA $NVMF_TARGET_IP 4262 IPv4" "$NVMF_TARGET_IP:4262" True
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode1/hosts create nqn.2014-08.org.spdk:cnode2" "nqn.2014-08.org.spdk:cnode2" True
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode1/namespaces create Malloc5" "Malloc5" True
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode1/namespaces create Malloc6" "Malloc6" True
timing_exit spdkcli_create_nvmf_config

timing_enter spdkcli_check_match
check_match
timing_exit spdkcli_check_match

timing_enter spdkcli_clear_nvmf_config
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode1/namespaces delete nsid=1" "Malloc3"
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode1/namespaces delete nsid=2" "Malloc4"
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode1/hosts delete nqn.2014-08.org.spdk:cnode2" "nqn.2014-08.org.spdk:cnode2"
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode1/listen_addresses delete RDMA $NVMF_TARGET_IP 4262" "$NVMF_TARGET_IP:4262"
$spdkcli_job "/nvmf/subsystem/nqn.2014-08.org.spdk:cnode1/listen_addresses delete RDMA $NVMF_TARGET_IP 4261" "$NVMF_TARGET_IP:4261"
$spdkcli_job "/nvmf/subsystem delete nqn.2014-08.org.spdk:cnode3" "nqn.2014-08.org.spdk:cnode3"
$spdkcli_job "/nvmf/subsystem delete nqn.2014-08.org.spdk:cnode2" "nqn.2014-08.org.spdk:cnode2"
$spdkcli_job "/nvmf/subsystem delete nqn.2014-08.org.spdk:cnode1" "nqn.2014-08.org.spdk:cnode1"
$spdkcli_job "/bdevs/malloc delete Malloc6" "Malloc6"
$spdkcli_job "/bdevs/malloc delete Malloc5" "Malloc5"
$spdkcli_job "/bdevs/malloc delete Malloc4" "Malloc4"
$spdkcli_job "/bdevs/malloc delete Malloc3" "Malloc3"
$spdkcli_job "/bdevs/malloc delete Malloc2" "Malloc2"
$spdkcli_job "/bdevs/malloc delete Malloc1" "Malloc1"
timing_exit spdkcli_clear_nvmf_config

killprocess $spdk_tgt_pid
#revert_soft_roce
timing_exit spdkcli_nvmf
report_test_completion spdk_cli_nvmf
