#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation.
#  All rights reserved.

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
	local cgroup pid
	local -A cgroups=()

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
		for pid in /proc/+([0-9]); do
			cgroup=$(get_cgroup "${pid##*/}") || continue
			[[ $cgroup != / ]] || continue
			cgroups["$cgroup"]=$cgroup
		done 2> /dev/null
		for cgroup in "${!cgroups[@]}"; do
			move_cgroup_procs "$cgroup" /
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

	# If target cgroups don't exist then there's nothing to do.
	[[ -e $sysfs_cgroup/$old_cgroup ]] || return 0
	[[ -e $sysfs_cgroup/$new_cgroup ]] || return 0

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

	local moved=0
	for proc in "${!procs[@]}"; do
		# We can't move every kernel thread around and every process can
		# exit at any point so ignore any failures upon writing the
		# processes out but keep count of any failed attempts for debugging
		# purposes.
		if move_proc "$proc" "$new_cgroup" "$old_cgroup" "$new_proc_interface"; then
			((++moved))
		fi
	done
	echo "Moved $moved processes, failed $((${#procs[@]} - moved))" >&2
}

move_proc() {
	local proc=$1 new_cgroup=$2 old_cgroup=${3:-N/A} attr=$4 write_fail out=/dev/stderr

	[[ -n $SILENT_CGROUP_DEBUG ]] && out=/dev/null

	echo "Moving $proc ($(id_proc "$proc" 2>&1)) to $new_cgroup from $old_cgroup" > "$out"
	if ! write_fail=$(set_cgroup_attr "$new_cgroup" "$attr" "$proc" 2>&1); then
		echo "Moving $proc failed: ${write_fail##*: }" > "$out"
		return 1
	fi
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

get_cgroup() {
	local pid=${1:-self} cgroup

	[[ -e /proc/$pid/cgroup ]] || return 1
	cgroup=$(< "/proc/$pid/cgroup")
	echo "${cgroup##*:}"
}

get_cgroup_path() {
	local cgroup

	cgroup=$(get_cgroup "$1") || return 1
	echo "$sysfs_cgroup$cgroup"
}

_set_cgroup_attr_top_bottom() {
	local cgroup_path=$1 attr=$2 val=$3

	if [[ -e ${cgroup_path%/*}/$attr ]]; then
		_set_cgroup_attr_top_bottom "${cgroup_path%/*}" "$attr" "$val"
	fi

	if [[ -e $cgroup_path/$attr ]]; then
		echo "$val" > "$cgroup_path/$attr"
	fi
}

set_cgroup_attr_top_bottom() {
	_set_cgroup_attr_top_bottom "$(get_cgroup_path "$1")" "$2" "$3"
}

id_proc() {
	local pid=$1 flag_to_check=${2:-all}
	local flags flags_map=() comm stats tflags

	[[ -e /proc/$pid/stat ]] || return 1
	# Comm is wrapped in () but the name of the thread itself may include "()", giving in result
	# something similar to: ((sd-pam))
	comm=$(< "/proc/$pid/stat") || return 1

	stats=(${comm/*) /}) tflags=${stats[6]}

	# include/linux/sched.h
	flags_map[0x1]=PF_VCPU
	flags_map[0x2]=PF_IDLE
	flags_map[0x4]=PF_EXITING
	flags_map[0x8]=PF_POSTCOREDUMP
	flags_map[0x10]=PF_IO_WORKER
	flags_map[0x20]=PF_WQ_WORKER
	flags_map[0x40]=PF_FORK_NO_EXEC
	flags_map[0x80]=PF_MCE_PROCESS
	flags_map[0x100]=PF_SUPERPRIV
	flags_map[0x200]=PF_DUMPCORE
	flags_map[0x400]=PF_SIGNALED
	flags_map[0x800]=PF_MEMALLOC
	flags_map[0x1000]=PF_NPROC_EXCEEDED
	flags_map[0x2000]=PF_USED_MATH
	flags_map[0x4000]=PF_USER_WORKER
	flags_map[0x8000]=PF_NOFREEZE
	flags_map[0x20000]=PF_KSWAPD
	flags_map[0x40000]=PF_MEMALLOC_NOFS
	flags_map[0x80000]=PF_MEMALLOC_NOIO
	flags_map[0x100000]=PF_LOCAL_THROTTLE
	flags_map[0x00200000]=PF_KTHREAD
	flags_map[0x00400000]=PF_RANDOMIZE
	flags_map[0x04000000]=PF_NO_SETAFFINITY
	flags_map[0x08000000]=PF_MCE_EARLY
	flags_map[0x10000000]=PF_MEMALLOC_PIN
	flags_map[0x80000000]=PF_SUSPEND_TASK

	for flag in "${!flags_map[@]}"; do
		[[ $flag_to_check == "${flags_map[flag]}" || $flag_to_check == all ]] || continue
		((tflags & flag)) && flags=${flags:+$flags,}"${flags_map[flag]}"
	done
	if [[ -n $flags ]]; then
		echo "$flags" >&2
		return 0
	fi
	return 1
}

declare -r sysfs_cgroup=/sys/fs/cgroup
cgroup_version=$(check_cgroup)
