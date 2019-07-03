#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh
rpc_py="$rootdir/scripts/rpc.py"

timing_enter nvmf_compress_bdev_setup
nvmftestinit
nvmfappstart "-m 0x7"

$rpc_py nvmf_create_transport -t $TEST_TRANSPORT -u 8192
# Construct a nvme bdev
bdf=$(iter_pci_class_code 01 08 02 | head -1)
base_bdev=$($rpc_py construct_nvme_bdev -b "Nvme0" -t "pcie" -a "$bdf")

# Create the logical volume store on the malloc bdev
lvs=$($rpc_py construct_lvol_store "$base_bdev" lvs)
#lvs=$($rpc_py construct_lvol_store Nvme0n1 lvs0)
lvb=$($rpc_py construct_lvol_bdev -t -u $lvs lvb0 10000)

# Construct compress bdev and use ISAL PMD
$rpc_py set_compress_pmd -p 2
compress_bdev=$($rpc_py construct_compress_bdev -b $lvb -p /tmp)

# Create an NVMe-oF subsystem and add compress bdev as a namespace
$rpc_py nvmf_subsystem_create nqn.2016-06.io.spdk:cnode0 -a -s SPDK0
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode0 $compress_bdev
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode0 -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# Start random read writes in the background
$rootdir/examples/nvme/perf/perf -r "trtype:$TEST_TRANSPORT adrfam:IPv4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" -o 4096 -q 128 -s 512 -w randrw -t 10 -c 0x18 -M 50 &
perf_pid=$!

# Wait for I/O to complete
wait $perf_pid

# Clean up
$rpc_py delete_nvmf_subsystem nqn.2016-06.io.spdk:cnode0
$rpc_py delete_compress_bdev $compress_bdev
$rpc_py destroy_lvol_bdev $lvb
$rpc_py destroy_lvol_store -u $lvs

 trap - SIGINT SIGTERM EXIT

 nvmftestfini
 timing_exit nvmf_compress_bdev_setup