#!/usr/bin/env bash

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../")

source "$rootdir/test/common/autotest_common.sh"
source "$testdir/common.sh"

trap 'killprocess "$spdk_pid" || :; restore_cpufreq' EXIT

restore_cpufreq() {
	local cpu

	for cpu in "$spdk_main_core" "${cpus[@]}"; do
		set_cpufreq "$cpu" "$main_core_min_freq" "$main_core_max_freq"
		set_cpufreq_governor "$cpu" "$initial_main_core_governor"
	done
}

update_main_core_cpufreq() {
	map_cpufreq

	main_core_driver=${cpufreq_drivers[spdk_main_core]}
	main_core_governor=${cpufreq_governors[spdk_main_core]}
	main_core_set_min_freq=${cpufreq_min_freqs[spdk_main_core]}
	main_core_set_cur_freq=${cpufreq_cur_freqs[spdk_main_core]}
	main_core_set_max_freq=${cpufreq_max_freqs[spdk_main_core]}

	if ((${#main_core_freqs[@]} == 0)); then
		main_core_freqs=("${!cpufreq_available_freqs[spdk_main_core]}")
		main_core_max_freq=${main_core_freqs[0]}
		main_core_min_freq=${main_core_freqs[-1]}
	fi
	if ((${#main_core_freqs_map[@]} == 0)); then
		fold_list_onto_array main_core_freqs_map "${main_core_freqs[@]}"
	fi

	case "$main_core_driver" in
		acpi-cpufreq) main_core_setspeed=${cpufreq_setspeed[spdk_main_core]} ;;
		intel_pstate | intel_cpufreq) main_core_setspeed=$main_core_set_max_freq ;;
		cppc_cpufreq) main_core_setspeed=${cpufreq_setspeed[spdk_main_core]} ;;
	esac
}

verify_dpdk_governor() {
	xtrace_disable

	# Run the app and see if DPDK's PM subsystem is doing the proper thing. Aim at the main core.
	# What we expect to happen is based mainly on what cpufreq driver is in use. The main assumption
	# is that with all SPDK threads being idle the governor should start lowering the frequency for
	# the main core.
	#  - acpi-cpufreq:
	#    - governor set to userspace
	#    - lowering setspeed for the main core
	#    - having setspeed at lowest supported frequency
	#  - intel_pstate (active or passive)
	#    - governor set to performance
	#    - lowering max_freq and min_freq for the main core
	#    - having max_freq and min_freq at lowest supported frequency
	#  - cppc_cpufreq:
	#    - governor set to userspace
	#    - lowering setspeed for the main core
	#    - having setspeed at lowest supported frequency

	local -g cpus

	local dir_map
	dir_map[0]="<"
	dir_map[1]=">"
	dir_map[2]="=="

	fold_list_onto_array cpus $(parse_cpu_list <(echo "$spdk_cpus_csv"))
	# Get rid of the main core
	unset -v "cpus[spdk_main_core]"

	local samples=0 all_set=0 dir=-1 old_main_core_setspeed=0

	exec_under_dynamic_scheduler "${SPDK_APP[@]}" -m "$spdk_cpumask" --main-core "$spdk_main_core"

	while ((all_set == 0 && samples++ <= 50)); do
		update_main_core_cpufreq

		if [[ $main_core_setspeed == "<unsupported>" ]]; then
			# governor hasn't taken over yet, skip this sample
			printf 'Waiting for DPDK governor to take over...\n'
			continue
		fi

		if ((main_core_setspeed > old_main_core_setspeed)); then
			dir=1
		elif ((main_core_setspeed < old_main_core_setspeed)); then
			dir=0
		elif ((main_core_setspeed == old_main_core_setspeed)); then
			# Frequency didn't change, skip and wait for a bit
			sleep 0.5s
			continue
		fi

		case "$main_core_driver" in
			acpi-cpufreq)
				[[ $main_core_governor == userspace ]] \
					&& [[ -n ${main_core_freqs_map[main_core_setspeed]} ]] \
					&& ((main_core_setspeed == main_core_freqs[-1])) \
					&& ((dir == 0))
				;;
			intel_pstate | intel_cpufreq)
				[[ $main_core_governor == performance ]] \
					&& [[ -n ${main_core_freqs_map[main_core_setspeed]} ]] \
					&& ((main_core_setspeed == main_core_freqs[-1])) \
					&& ((main_core_set_max_freq == main_core_set_min_freq)) \
					&& ((dir == 0))
				;;
			cppc_cpufreq)
				[[ $main_core_governor == userspace ]] \
					&& [[ -n ${main_core_freqs_map[main_core_setspeed]} ]] \
					&& ((main_core_setspeed == main_core_freqs[-1])) \
					&& ((dir == 0))
				;;
		esac && all_set=1

		# Print stats after first sane sample was taken
		if ((old_main_core_setspeed != 0 && dir != -1)); then
			# Currently, we only determine if DPDK set sysfs attributes to min frequency supported by given cpu.
			# We don't act on actual frequency cpu ends up spinning at. The reason being all sibling threads of
			# the main cpu would need to be adjusted manually (i.e. their cpufreq attributes) in order for freq
			# to start going down. This is deemed as not necessary for the moment since the core objective of
			# the test is to determine if DPDK's governor did its job, within its current scope, not how it
			# impacts the system overall.
			printf 'MAIN DPDK cpu%u current frequency at %u KHz (%u-%u KHz), set frequency %u KHz %s %u KHz\n' \
				"$spdk_main_core" "$main_core_set_cur_freq" "$main_core_min_freq" "$main_core_max_freq" \
				"$main_core_setspeed" "${dir_map[dir]}" "$old_main_core_setspeed"
		else
			printf 'Waiting for samples...\n'
		fi

		old_main_core_setspeed=$main_core_setspeed
	done

	((all_set == 1))

	xtrace_restore
}

map_cpufreq
# Save initial scaling governor to restore it later on
initial_main_core_governor=${cpufreq_governors[spdk_main_core]}

verify_dpdk_governor
