#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation.
#  All rights reserved.

# Select cores for the test
xtrace_disable

source "$testdir/common.sh"

restore_cgroups() {
	xtrace_disable
	remove_cpuset_cgroup
	xtrace_restore
}

trap "restore_cgroups" EXIT

# Number of cpus to include in the mask
NUM_CPUS=${NUM_CPUS:-8}

init_cpuset_cgroup
map_cpus

# Build core mask. Avoid all CPUs that may be offline and skip cpu0
# (and all its potential thread siblings) as it's already doing an
# extra work for the kernel.
denied_list $(get_cpus "${cpu_node_map[0]}" "${cpu_core_map[0]}")
# If there are any isolated cpus (as defined on the kernel cmdline
# with isolcpus) they take the priority. We fill up the list up to
# NUM_CPUS, applying filtering as per the denied list. All cpus are
# taken from node0.
allowed_list "$NUM_CPUS" 0

# Assign proper resources to the cpuset/spdk
spdk_cpus=("${allowed[@]}")
spdk_cpus_csv=$(fold_array_onto_string "${spdk_cpus[@]}")
spdk_cpumask=$(mask_cpus "${spdk_cpus[@]}")
spdk_main_core=${spdk_cpus[0]}
spdk_cpus_mems=0

# Build list of remaining cpus for posterity
denied_list "${spdk_cpus[@]}"
fold_list_onto_array allowed "${cpus[@]}"
filter_allowed_list

all_cpus=("${allowed[@]}")
all_cpus_csv=$(fold_array_onto_string "${all_cpus[@]}")
all_cpumask=$(mask_cpus "${all_cpus[@]}")
all_cpus_mems=0

# For cgroupv2 it's required we jump first to the root cgroup ...
move_proc "$$" "/" "" cgroup.procs
# ... so we can now settle in a dedicated cgroup /cpuset
move_proc "$$" "/cpuset" "" cgroup.procs

set_cgroup_attr "/cpuset" cpuset.cpus "$spdk_cpus_csv"
set_cgroup_attr "/cpuset" cpuset.mems "$spdk_cpus_mems"

export \
	"spdk_cpumask=$spdk_cpumask" \
	"spdk_cpus_csv=$spdk_cpus_csv" \
	"spdk_cpus_no=${#spdk_cpus[@]}" \
	"spdk_main_core=$spdk_main_core" \
	"all_cpumask=$all_cpumask" \
	"all_cpus_csv=$all_cpus_csv"

xtrace_restore
