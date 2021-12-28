# Select cores for the test
xtrace_disable

source "$testdir/common.sh"

restore_cgroups() {
	xtrace_disable
	kill_in_cgroup "/cpuset/spdk"
	remove_cgroup "/cpuset/spdk"
	remove_cgroup "/cpuset/all" || true
	remove_cpuset_cgroup || true
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

# Pin spdk cores to a new cgroup
create_cgroup "/cpuset/spdk"
create_cgroup "/cpuset/all"
set_cgroup_attr "/cpuset/spdk" cpuset.cpus "$spdk_cpus_csv"
set_cgroup_attr "/cpuset/spdk" cpuset.mems "$spdk_cpus_mems"
set_cgroup_attr "/cpuset/all" cpuset.cpus "$all_cpus_csv"
set_cgroup_attr "/cpuset/all" cpuset.mems "$all_cpus_mems"
move_cgroup_procs "/cpuset" "/cpuset/all"

export \
	"spdk_cpumask=$spdk_cpumask" \
	"spdk_cpus_csv=$spdk_cpus_csv" \
	"spdk_cpus_no=${#spdk_cpus[@]}" \
	"spdk_main_core=$spdk_main_core" \
	"all_cpumask=$all_cpumask" \
	"all_cpus_csv=$all_cpus_csv"

xtrace_restore
