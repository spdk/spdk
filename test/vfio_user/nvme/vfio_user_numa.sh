#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vfio_user/common.sh
source $rootdir/test/vfio_user/nvme/common.sh
source $rootdir/test/vfio_user/autotest.config

rpc_py="$rootdir/scripts/rpc.py -s $(get_vhost_dir 0)/rpc.sock"
nqn="nqn.2019-07.io.spdk"

trap 'clean_vfio_user' EXIT
vhosttestinit
vfio_user_run 0

vm_muser_dir="$VM_DIR/0/muser"
rm -rf $vm_muser_dir
mkdir -p $vm_muser_dir/domain/muser0/0

function get_listener_numa_id() {
	local sub_nqn="$1"

	$rpc_py framework_get_config nvmf | jq -r --arg sub_nqn "$sub_nqn" \
		'.[] | select(.method=="nvmf_subsystem_add_listener" and .params.nqn==$sub_nqn) | .params.numa_id'
}

# Create Malloc device without NUMA node ID assignment
$rpc_py bdev_malloc_create 1 512 -n -1 -b Malloc_ANY
$rpc_py nvmf_create_subsystem "$nqn:cnode_ANY" -a -s SPDK0
$rpc_py nvmf_subsystem_add_ns "$nqn:cnode_ANY" Malloc_ANY

# Create Malloc device with NUMA node ID assigned to NUMA 0
$rpc_py bdev_malloc_create 1 512 -n 0 -b Malloc_ZERO
$rpc_py nvmf_create_subsystem "$nqn:cnode_ZERO" -a -s SPDK0
$rpc_py nvmf_subsystem_add_ns "$nqn:cnode_ZERO" Malloc_ZERO

# Start listener for a subsystem with implicit ANY NUMA node ID
$rpc_py nvmf_subsystem_add_listener -t VFIOUSER -a "$vm_muser_dir" -s 0 "$nqn:cnode_ANY"
[[ "$(get_listener_numa_id "$nqn:cnode_ANY")" = "-1" ]]
$rpc_py nvmf_subsystem_remove_listener -t VFIOUSER -a "$vm_muser_dir" -s 0 "$nqn:cnode_ANY"

# Start listener for a subsystem with explicit ANY NUMA node ID
$rpc_py nvmf_subsystem_add_listener -t VFIOUSER -a "$vm_muser_dir" -s 0 "$nqn:cnode_ANY" --numa-id "-1"
[[ "$(get_listener_numa_id "$nqn:cnode_ANY")" = "-1" ]]

# Add namespace with zero NUMA to subsystem that had ANY NUMA node ID
$rpc_py bdev_malloc_create 1 512 -n -1 -b Malloc_ZERO_2
$rpc_py nvmf_subsystem_add_ns "$nqn:cnode_ANY" Malloc_ZERO_2
[[ "$(get_listener_numa_id "$nqn:cnode_ANY")" = "-1" ]]
$rpc_py nvmf_subsystem_remove_listener -t VFIOUSER -a "$vm_muser_dir" -s 0 "$nqn:cnode_ANY"

# Start listener for a subsystem with zero NUMA node ID
$rpc_py nvmf_subsystem_add_listener -t VFIOUSER -a "$vm_muser_dir" -s 0 "$nqn:cnode_ZERO" --numa-id "0"
[[ "$(get_listener_numa_id "$nqn:cnode_ZERO")" = "0" ]]

# Add namespace with ANY NUMA to subsystem that had zero NUMA node ID
$rpc_py bdev_malloc_create 1 512 -n -1 -b Malloc_ANY_2
NOT $rpc_py nvmf_subsystem_add_ns "$nqn:cnode_ZERO" Malloc_ANY_2
[[ "$(get_listener_numa_id "$nqn:cnode_ZERO")" = "0" ]]
$rpc_py nvmf_subsystem_remove_listener -t VFIOUSER -a "$vm_muser_dir" -s 0 "$nqn:cnode_ZERO"

# Start listener that requires NUMA node ID unavailable on the system
NOT $rpc_py nvmf_subsystem_add_listener -t VFIOUSER -a "$vm_muser_dir" -s 0 "$nqn:cnode_ANY" --numa-id "42"

vhost_kill 0
