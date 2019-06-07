#!/usr/bin/env bash
set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
rpc_py=$rootdir/scripts/rpc.py
source $rootdir/test/nvmf/common.sh

nvmftestinit

function error_exit {
        killprocess $spdk_tgt_pid
        rm conf.json
}

trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR

timing_enter run_spdk_tgt
$rootdir/scripts/gen_nvme.sh >> conf.json
$rootdir/app/spdk_tgt/spdk_tgt -m 0x3 -p 0 -s 1024 -c conf.json &
spdk_tgt_pid=$!
waitforlisten $spdk_tgt_pid
$rpc_py set_bdev_nvme_hotplug -e
timing_exit run_spdk_tgt

$rpc_py get_bdevs
$rpc_py nvmf_create_transport -t RDMA -u 8192
$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Nvme0n1
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t rdma -a 10.0.2.15 -s 4260
nvme connect -t rdma -n nqn.2016-06.io.spdk:cnode1 -s 4260 -a 10.0.2.15

killprocess $spdk_tgt_pid
rm conf.json
