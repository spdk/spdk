check_cgroup() {
	# Try to work with both, cgroup-v1 and cgroup-v2. Verify which version is
	# in use by looking up interfaces common for either of the versions.
	if [[ -e $sysfs_cgroup/cgroup.controllers ]]; then
		# cgroup2 is mounted, check if cpuset controller is available
		[[ $(< "$sysfs_cgroup/cgroup.controllers") == *cpuset* ]] && echo 2
	elif [[ -e $sysfs_cgroup/cpuset/tasks ]]; then
		# cgroup's cpuset subsystem is mounted
		echo 1
	fi || return 1
}

init_cpuset_cgroup() {
	local cgroup

	# For cgroup-v2 we need to prepare cpuset subsystem on our own
	if ((cgroup_version == 2)); then
		set_cgroup_attr / cgroup.subtree_control "+cpuset"
		create_cgroup /cpuset
		set_cgroup_attr /cpuset cgroup.subtree_control "+cpuset"
		# On distros which use cgroup-v2 under systemd, each process is
		# maintained under separate, pre-configured subtree. With the rule of
		# "internal processes are not permitted" this means that we won't find
		# ourselves under subsystem's root, rather on the bottom of the cgroup
		# maintaining user's session. To recreate the simple /cpuset setup from
		# v1, move all the threads from all the existing cgroups to the top
		# cgroup / and then migrate it to the /cpuset we created above.
		for cgroup in /proc/+([0-9])/cgroup; do
			[[ -e $cgroup ]] || continue
			cgroup=$(< "$cgroup") cgroup=${cgroup##*:}
			[[ $cgroup != / ]] || continue
			move_cgroup_procs "${cgroup##*:}" /
		done
		# Now, move all the threads to the cpuset
		move_cgroup_procs / /cpuset
	elif ((cgroup_version == 1)); then
		set_cgroup_attr /cpuset cgroup.procs "$$"
	fi
}

is_cgroup_threaded() {
	[[ -e $sysfs_cgroup/$1/cgroup.type ]] || return 1
	[[ $(< "$sysfs_cgroup/$1/cgroup.type") == threaded ]]
}

move_cgroup_procs() {
	local old_cgroup=$1
	local new_cgroup=$2
	local proc procs old_proc_interface new_proc_interface

	[[ -e $sysfs_cgroup/$old_cgroup ]] || return 1
	[[ -e $sysfs_cgroup/$new_cgroup ]] || return 1

	old_proc_interface=cgroup.procs
	new_proc_interface=cgroup.procs
	if ((cgroup_version == 2)); then
		if is_cgroup_threaded "$new_cgroup"; then
			new_proc_interface=cgroup.threads
		fi
		if is_cgroup_threaded "$old_cgroup"; then
			old_proc_interface=cgroup.threads
		fi
	fi

	fold_list_onto_array procs $(< "$sysfs_cgroup/$old_cgroup/$old_proc_interface")

	for proc in "${!procs[@]}"; do
		# We can't move every kernel thread around and every process can
		# exit at any point so ignore any failures upon writing the
		# processes out. FIXME: Check PF_KTHREAD instead?
		[[ -n $(readlink -f "/proc/$proc/exe") ]] || continue
		echo "$proc" > "$sysfs_cgroup/$new_cgroup/$new_proc_interface" 2> /dev/null || :
	done
}

set_cgroup_attr() {
	local cgroup=$1
	local attr=$2
	local val=$3

	[[ -e $sysfs_cgroup/$cgroup/$attr ]] || return 1

	if [[ -n $val ]]; then
		echo "$val" > "$sysfs_cgroup/$cgroup/$attr"
	fi
}

create_cgroup() {
	[[ ! -e $sysfs_cgroup/$1 ]] || return 0
	mkdir "$sysfs_cgroup/$1"
	if ((cgroup_version == 2)); then
		echo "threaded" > "$sysfs_cgroup/$1/cgroup.type"
	fi
}

remove_cgroup() {
	local root_cgroup
	root_cgroup=$(dirname "$1")

	[[ -e $sysfs_cgroup/$1 ]] || return 0
	move_cgroup_procs "$1" "$root_cgroup"
	rmdir "$sysfs_cgroup/$1"
}

exec_in_cgroup() {
	# Run this function as a background job - the reason why it remains {} instead
	# of being declared as a subshell is to avoid having an extra bash fork around
	# - note the exec call.

	local cgroup=$1
	local proc_interface=cgroup.procs

	shift || return 1

	if ((cgroup_version == 2)) && is_cgroup_threaded "$cgroup"; then
		proc_interface=cgroup.threads
	fi
	set_cgroup_attr "$cgroup" "$proc_interface" "$BASHPID"
	exec "$@"
}

kill_in_cgroup() {
	local cgroup=$1
	local pid=$2
	local proc_interface=cgroup.procs
	local cgroup_pids

	if ((cgroup_version == 2)) && is_cgroup_threaded "$cgroup"; then
		proc_interface=cgroup.threads
	fi

	fold_list_onto_array \
		cgroup_pids \
		$(< "$sysfs_cgroup/$cgroup/$proc_interface")

	if [[ -n $pid ]]; then
		if [[ -n ${cgroup_pids[pid]} ]]; then
			kill "$pid"
		fi
	elif ((${#cgroup_pids[@]} > 0)); then
		kill "${cgroup_pids[@]}"
	fi
}

remove_cpuset_cgroup() {
	if ((cgroup_version == 2)); then
		remove_cgroup /cpuset
	fi
}

declare -r sysfs_cgroup=/sys/fs/cgroup
cgroup_version=$(check_cgroup)
