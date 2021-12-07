shopt -s nullglob extglob

declare -r sysfs_system=/sys/devices/system
declare -r sysfs_cpu=$sysfs_system/cpu
declare -r sysfs_node=$sysfs_system/node

declare -r scheduler=$rootdir/test/event/scheduler/scheduler
declare -r plugin=scheduler_plugin

source "$testdir/cgroups.sh"

fold_list_onto_array() {
	local array=$1
	local elem

	shift || return 0

	for elem; do
		eval "${array}[elem]=$elem"
	done
}

fold_array_onto_string() {
	local cpus=("$@")

	local IFS=","
	echo "${cpus[*]}"
}

parse_cpu_list() {
	local list=$1
	local elem elems cpus

	# 0-2,4,6-9, etc.
	IFS="," read -ra elems < "$list"

	((${#elems[@]} > 0)) || return 0

	for elem in "${elems[@]}"; do
		if [[ $elem == *-* ]]; then
			local start=${elem%-*} end=${elem#*-}
			while ((start <= end)); do
				cpus[start++]=$start
			done
		else
			cpus[elem]=$elem
		fi
	done
	printf '%u\n' "${!cpus[@]}"
}

map_cpus_node() {
	local node_idx=$1
	local -n _cpu_node_map=node_${node_idx}_cpu
	local cpu_idx core_idx

	for cpu_idx in $(parse_cpu_list "$sysfs_node/node$node_idx/cpulist"); do
		if is_cpu_online "$cpu_idx"; then
			core_idx=$(< "$sysfs_cpu/cpu$cpu_idx/topology/core_id")
			local -n _cpu_core_map=node_${node_idx}_core_${core_idx}
			_cpu_core_map+=("$cpu_idx") cpu_core_map[cpu_idx]=$core_idx
		fi
		_cpu_node_map+=("$cpu_idx") cpu_node_map[cpu_idx]=$node_idx
		cpus+=("$cpu_idx")
	done

	nodes[node_idx]=$node_idx
}

map_cpus() {
	local -g cpus=()
	local -g nodes=()
	local -g cpu_node_map=()
	local -g cpu_core_map=()
	local -g core_node_map=()
	local node

	unset -v "${!node_@}"

	for node in "$sysfs_node/node"+([0-9]); do
		map_cpus_node "${node##*node}"
	done
}

get_cpus() {
	local node=$1
	local core=$2
	local _cpus

	if [[ -z $node ]]; then
		_cpus=("${cpus[@]}")
	elif [[ -n $node ]]; then
		eval "_cpus=(\${node_${node}_cpu[@]})"
		if [[ -n $core ]]; then
			eval "_cpus=(\${node_${node}_core_${core}[@]})"
		fi
	fi
	((${#_cpus[@]} > 0)) || return 1
	printf '%u\n' "${_cpus[@]}"
}

get_isolated_cpus() {
	[[ -e $sysfs_cpu/isolated ]] || return 0
	parse_cpu_list "$sysfs_cpu/isolated"
}

get_offline_cpus() {
	local offline

	[[ -e $sysfs_cpu/offline ]] || return 0
	parse_cpu_list "$sysfs_cpu/offline"
}

get_online_cpus() {
	[[ -e $sysfs_cpu/online ]] || return 0
	parse_cpu_list "$sysfs_cpu/online"
}

is_cpu_online() {
	local online

	fold_list_onto_array online $(get_online_cpus)
	[[ -v online[$1] ]]
}

is_cpu_offline() {
	! is_cpu_online "$1"
}

online_cpu() {
	is_cpu_offline "$1" || return 0
	[[ -e $sysfs_cpu/cpu$1/online ]] && echo 1 > "$sysfs_cpu/cpu$1/online"
}

offline_cpu() {
	is_cpu_online "$1" || return 0
	[[ -e $sysfs_cpu/cpu$1/online ]] && echo 0 > "$sysfs_cpu/cpu$1/online"
}

mask_cpus() {
	local cpu
	local mask=0

	for cpu; do
		((mask |= 1 << cpu))
	done
	printf '0x%x\n' "$mask"
}

denied_list() {
	local -g denied

	fold_list_onto_array denied $(get_offline_cpus) "$@"
}

filter_allowed_list() {
	local cpu

	for cpu in "${!allowed[@]}"; do
		if [[ -n ${denied[cpu]} ]]; then
			unset -v "allowed[cpu]"
		fi
	done
}

allowed_list() {
	local max=${1:-4}
	local node=${2:-0}
	local cpu_count=${cpu_count:--1}

	local -g allowed

	fold_list_onto_array allowed $(get_isolated_cpus)

	if ((cpu_count < 0 && ${#allowed[@]} > 0)); then
		((max += ${#allowed[@]}))
	fi

	local -n node_cpu_ref=node_${node}_cpu

	while ((${#allowed[@]} < max && ++cpu_count < ${#node_cpu_ref[@]})); do
		fold_list_onto_array allowed $(get_cpus "$node" "${cpu_core_map[node_cpu_ref[cpu_count]]}")
	done

	filter_allowed_list

	if ((${#allowed[@]} == max)); then
		return 0
	elif ((cpu_count == ${#node_cpu_ref[@]})); then
		return 0
	else
		allowed_list "$max" "$node"
	fi
}

get_proc_cpu_affinity() {
	xtrace_disable

	local pid=${1:-$$}
	local status val

	[[ -e /proc/$pid/status ]] || return 1
	while IFS=":"$'\t' read -r status val; do
		if [[ $status == Cpus_allowed_list ]]; then
			parse_cpu_list <(echo "$val")
			return 0
		fi
	done < "/proc/$pid/status"

	xtrace_restore
}

map_cpufreq() {
	# This info is used to cross-reference current cpufreq setup with
	# what DPDK's governor actually puts in place.

	local -g cpufreq_drivers=()
	local -g cpufreq_governors=()
	local -g cpufreq_base_freqs=()
	local -g cpufreq_max_freqs=()
	local -g cpufreq_min_freqs=()
	local -g cpufreq_cur_freqs=()
	local -g cpufreq_is_turbo=()
	local -g cpufreq_available_freqs=()
	local -g cpufreq_available_governors=()
	local -g cpufreq_high_prio=()
	local -g cpufreq_non_turbo_ratio=()
	local -g cpufreq_setspeed=()
	local -g cpuinfo_max_freqs=()
	local -g cpuinfo_min_freqs=()
	local -g turbo_enabled=0
	local cpu cpu_idx

	for cpu in "$sysfs_cpu/cpu"+([0-9]); do
		cpu_idx=${cpu##*cpu}
		[[ -e $cpu/cpufreq ]] || continue
		cpufreq_drivers[cpu_idx]=$(< "$cpu/cpufreq/scaling_driver")
		cpufreq_governors[cpu_idx]=$(< "$cpu/cpufreq/scaling_governor")

		# In case HWP is on
		if [[ -e $cpu/cpufreq/base_frequency ]]; then
			cpufreq_base_freqs[cpu_idx]=$(< "$cpu/cpufreq/base_frequency")
		fi

		cpufreq_cur_freqs[cpu_idx]=$(< "$cpu/cpufreq/scaling_cur_freq")
		cpufreq_max_freqs[cpu_idx]=$(< "$cpu/cpufreq/scaling_max_freq")
		cpufreq_min_freqs[cpu_idx]=$(< "$cpu/cpufreq/scaling_min_freq")

		local -n available_governors=available_governors_cpu_${cpu_idx}
		cpufreq_available_governors[cpu_idx]="available_governors_cpu_${cpu_idx}[@]"
		available_governors=($(< "$cpu/cpufreq/scaling_available_governors"))

		local -n available_freqs=available_freqs_cpu_${cpu_idx}
		cpufreq_available_freqs[cpu_idx]="available_freqs_cpu_${cpu_idx}[@]"

		case "${cpufreq_drivers[cpu_idx]}" in
			acpi-cpufreq)
				available_freqs=($(< "$cpu/cpufreq/scaling_available_frequencies"))
				if ((available_freqs[0] - 1000 == available_freqs[1])); then
					cpufreq_is_turbo[cpu_idx]=1
				else
					cpufreq_is_turbo[cpu_idx]=0
				fi
				cpufreq_setspeed[cpu_idx]=$(< "$cpu/cpufreq/scaling_setspeed")
				;;
			intel_pstate | intel_cpufreq) # active or passive
				local non_turbo_ratio base_max_freq num_freq freq is_turbo=0

				non_turbo_ratio=$("$testdir/rdmsr.pl" "$cpu_idx" 0xce)
				cpuinfo_min_freqs[cpu_idx]=$(< "$cpu/cpufreq/cpuinfo_min_freq")
				cpuinfo_max_freqs[cpu_idx]=$(< "$cpu/cpufreq/cpuinfo_max_freq")
				cpufreq_non_turbo_ratio[cpu_idx]=$(((non_turbo_ratio >> 8) & 0xff))
				if ((cpufreq_base_freqs[cpu_idx] / 100000 > cpufreq_non_turbo_ratio[cpu_idx])); then
					cpufreq_high_prio[cpu_idx]=1
					base_max_freq=${cpufreq_base_freqs[cpu_idx]}
				else
					cpufreq_high_prio[cpu_idx]=0
					base_max_freq=$((cpufreq_non_turbo_ratio[cpu_idx] * 100000))
				fi
				num_freqs=$(((base_max_freq - cpuinfo_min_freqs[cpu_idx]) / 100000 + 1))
				if ((base_max_freq < cpuinfo_max_freqs[cpu_idx])); then
					((num_freqs += 1))
					cpufreq_is_turbo[cpu_idx]=1
				else
					cpufreq_is_turbo[cpu_idx]=0
				fi
				available_freqs=()
				for ((freq = 0; freq < num_freqs; freq++)); do
					if ((freq == 0 && cpufreq_is_turbo[cpu_idx] == 1)); then
						available_freqs[freq]=$((base_max_freq + 1))
					else
						available_freqs[freq]=$((base_max_freq - (freq - cpufreq_is_turbo[cpu_idx]) * 100000))
					fi
				done
				;;
			cppc_cpufreq)
				cpufreq_setspeed[cpu_idx]=$(< "$cpu/cpufreq/scaling_setspeed")
				scaling_min_freqs[cpu_idx]=$(< "$cpu/cpufreq/scaling_min_freq")
				scaling_max_freqs[cpu_idx]=$(< "$cpu/cpufreq/scaling_max_freq")
				cpuinfo_max_freqs[cpu_idx]=$(< "$cpu/cpufreq/cpuinfo_max_freq")
				nominal_perf[cpu_idx]=$(< "$cpu/acpi_cppc/nominal_perf")
				highest_perf[cpu_idx]=$(< "$cpu/acpi_cppc/highest_perf")

				#the unit of highest_perf and nominal_perf differs on different arm platforms.
				#For highest_perf, it maybe 300 or 3000000, both means 3.0GHz.
				if ((highest_perf[cpu_idx] > nominal_perf[cpu_idx] && (\
					highest_perf[cpu_idx] == cpuinfo_max_freqs[cpu_idx] || \
					highest_perf[cpu_idx] * 10000 == cpuinfo_max_freqs[cpu_idx]))); then
					cpufreq_is_turbo[cpu_idx]=1
				else
					cpufreq_is_turbo[cpu_idx]=0
				fi

				if ((nominal_perf[cpu_idx] < 10000)); then
					nominal_perf[cpu_idx]=$((nominal_perf[cpu_idx] * 10000))
				fi

				num_freqs=$(((nominal_perf[cpu_idx] - scaling_min_freqs[cpu_idx]) / 100000 + 1 + \
					cpufreq_is_turbo[cpu_idx]))

				available_freqs=()
				for ((freq = 0; freq < num_freqs; freq++)); do
					if ((freq == 0 && cpufreq_is_turbo[cpu_idx] == 1)); then
						available_freqs[freq]=$((scaling_max_freqs[cpu_idx]))
					else
						available_freqs[freq]=$((nominal_perf[cpu_idx] - (\
							freq - cpufreq_is_turbo[cpu_idx]) * 100000))
					fi
				done
				;;
		esac
	done
	if [[ -e $sysfs_cpu/cpufreq/boost ]]; then
		turbo_enabled=$(< "$sysfs_cpu/cpufreq/boost")
	elif [[ -e $sysfs_cpu/intel_pstate/no_turbo ]]; then
		turbo_enabled=$((!$(< "$sysfs_cpu/intel_pstate/no_turbo")))
	fi
}

set_cpufreq() {
	local cpu=$1
	local min_freq=$2
	local max_freq=$3
	local cpufreq=$sysfs_cpu/cpu$cpu/cpufreq

	# Map the cpufreq info first
	[[ -n ${cpufreq_drivers[cpu]} ]] || return 1
	[[ -n $min_freq ]] || return 1

	case "${cpufreq_drivers[cpu]}" in
		acpi-cpufreq)
			if [[ ${cpufreq_governors[cpu]} != userspace ]]; then
				echo "userspace" > "$cpufreq/scaling_governors"
			fi
			echo "$min_freq" > "$cpufreq/scaling_setspeed"
			;;
		intel_pstate | intel_cpufreq)
			if ((min_freq <= cpufreq_max_freqs[cpu])); then
				echo "$min_freq" > "$cpufreq/scaling_min_freq"
			fi
			if [[ -n $max_freq ]] && ((max_freq >= min_freq)); then
				echo "$max_freq" > "$cpufreq/scaling_max_freq"
			fi
			;;
	esac
}

set_cpufreq_governor() {
	local cpu=$1
	local governor=$2
	local cpufreq=$sysfs_cpu/cpu$cpu/cpufreq

	if [[ $(< "$cpufreq/scaling_governor") != "$governor" ]]; then
		echo "$governor" > "$cpufreq/scaling_governor"
	fi
}

exec_under_dynamic_scheduler() {
	if [[ -e /proc/$spdk_pid/status ]]; then
		killprocess "$spdk_pid"
	fi
	exec_in_cgroup "/cpuset/spdk" "$@" --wait-for-rpc &
	spdk_pid=$!
	# Give some time for the app to init itself
	waitforlisten "$spdk_pid"
	"$rootdir/scripts/rpc.py" framework_set_scheduler dynamic
	"$rootdir/scripts/rpc.py" framework_start_init
}

get_thread_stats() {
	xtrace_disable
	_get_thread_stats busy idle
	xtrace_restore
}

_get_thread_stats() {
	local list_busy=$1
	local list_idle=$2
	local thread threads stats

	stats=$(rpc_cmd thread_get_stats | jq -r '.threads[]')
	threads=($(jq -r '.id' <<< "$stats"))

	for thread in "${threads[@]}"; do
		eval "${list_busy}[$thread]=\$(jq -r \"select(.id == $thread) | .busy\" <<< \$stats)"
		eval "${list_idle}[$thread]=\$(jq -r \"select(.id == $thread) | .idle\" <<< \$stats)"
		thread_map[thread]=$(jq -r "select(.id == $thread) | .name" <<< "$stats")
	done
}

get_cpu_stat() {
	local cpu_idx=$1
	local stat=$2 stats astats

	while read -r cpu stats; do
		[[ $cpu == "cpu$cpu_idx" ]] && astats=($stats)
	done < /proc/stat

	case "$stat" in
		idle) echo "${astats[3]}" ;;
		all) printf '%u\n' "${astats[@]}" ;;
		*) ;;
	esac
}

create_thread() {
	rpc_cmd --plugin "$plugin" scheduler_thread_create "$@"
}

destroy_thread() {
	rpc_cmd --plugin "$plugin" scheduler_thread_delete "$@"
}

active_thread() {
	rpc_cmd --plugin "$plugin" scheduler_thread_set_active "$@"
}

get_cpu_time() {
	xtrace_disable

	local interval=$1 cpu_time=$2 interval_count
	shift 2
	local cpus=("$@") cpu
	local stats stat old_stats avg_load
	local total_sample

	# Exposed for the caller
	local -g cpu_times=()
	local -g avg_cpu_time=()

	# cpu_time:
	# 0 - user (time spent in user mode)
	# 1 - nice (Time spent in user mode with low priority)
	# 2 - system (Time spent in system mode)
	# 3 - idle (Time spent in the idle task)
	# 4 - iowait (Time waiting for I/O to complete)
	# 5 - irq (Time servicing interrupts)
	# 6 - softirq (Time servicing softirqs)
	# 7 - steal (Stolen time)
	# 8 - guest (Time spent running a virtual CPU)
	# 9 - guest_nice (Time spent running a niced guest)

	local -A cpu_time_map
	cpu_time_map["user"]=0
	cpu_time_map["nice"]=1
	cpu_time_map["system"]=2
	cpu_time_map["idle"]=3
	cpu_time_map["iowait"]=4
	cpu_time_map["irq"]=5
	cpu_time_map["softirq"]=6
	cpu_time_map["steal"]=7
	cpu_time_map["guest"]=8
	cpu_time_map["guest_nice"]=9

	# Clear up the env
	unset -v ${!stat_@}
	unset -v ${!old_stat_@}
	unset -v ${!avg_stat@}
	unset -v ${!avg_load@}

	cpu_time=${cpu_time_map["$cpu_time"]:-3}
	interval=$((interval <= 0 ? 1 : interval))
	# We skip first sample to have min 2 for stat comparison
	interval=$((interval + 1)) interval_count=0
	while ((interval_count++, --interval >= 0)); do
		for cpu in "${cpus[@]}"; do
			local -n old_stats=old_stats_$cpu
			local -n avg_load=avg_load_$cpu
			sample_stats=() total_sample=0

			stats=($(get_cpu_stat "$cpu" all))
			if ((interval_count == 1)); then
				# Skip first sample
				old_stats=("${stats[@]}")
				continue
			fi
			for stat in "${!stats[@]}"; do
				avg_load[stat]="stat_${stat}_${cpu}[@]"
				sample_stats[stat]=$((stats[stat] - old_stats[stat]))
				: $((total_sample += sample_stats[stat]))
			done
			for stat in "${!stats[@]}"; do
				local -n avg_stat=stat_${stat}_${cpu}
				avg_stat+=($((sample_stats[stat] * 100 / (total_sample == 0 ? 1 : total_sample))))
			done
			old_stats=("${stats[@]}")
		done
		sleep 1s
	done

	# We collected % for each time. Now determine the avg % for requested time.
	local load stat_load
	for cpu in "${cpus[@]}"; do
		load=0
		local -n avg_load_cpu=avg_load_$cpu
		stat_load=("${!avg_load_cpu[cpu_time]}")
		for stat in "${stat_load[@]}"; do
			: $((load += stat))
		done
		cpu_times[cpu]=${stat_load[*]}
		avg_cpu_time[cpu]=$((load / ${#stat_load[@]}))
	done

	xtrace_restore
}

collect_cpu_idle() {
	((${#cpus_to_collect[@]} > 0)) || return 1

	local time=${1:-5}
	local cpu
	local samples
	local -g is_idle=()

	printf 'Collecting cpu idle stats (cpus: %s) for %u seconds...\n' \
		"${cpus_to_collect[*]}" "$time"

	get_cpu_time "$time" idle "${cpus_to_collect[@]}"

	for cpu in "${cpus_to_collect[@]}"; do
		samples=(${cpu_times[cpu]})
		printf '* cpu%u idle samples: %s (avg: %u%%)\n' \
			"$cpu" "${samples[*]}" "${avg_cpu_time[cpu]}"
		# Cores with polling reactors have 0% idle time,
		# while the ones in interrupt mode won't have 100% idle.
		# Work can be potentially be scheduled to the core by kernel,
		# to prevent that affecting tests set reasonably high idle limit.
		# Consider last sample
		if ((samples[-1] >= 70)); then
			printf '* cpu%u is idle\n' "$cpu"
			is_idle[cpu]=1
		else
			printf '*cpu%u is not idle\n' "$cpu"
			is_idle[cpu]=0
		fi
	done
}

update_thread_cpus_map() {
	local cpu
	local -g thread_cpus=()
	local reactor_framework

	((${#cpus[@]} > 0)) || return 1

	get_thread_stats

	reactor_framework=$(rpc_cmd framework_get_reactors | jq -r '.reactors[]')
	for cpu in "${cpus[@]}"; do
		for thread in $(jq -r "select(.lcore == $cpu) | .lw_threads[].id" <<< "$reactor_framework"); do
			printf '* Thread %u (%s) on cpu%u\n' "$thread" "${thread_map[thread]}" "$cpu"
			thread_cpus[thread]=$cpu
		done
	done
	((${#thread_cpus[@]} > 0))
}
