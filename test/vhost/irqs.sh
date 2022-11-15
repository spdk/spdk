#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

source "$rootdir/test/scheduler/common.sh"

declare -a irqs_counters=()
declare -a irqs_devices=()
declare -a irqs_types=()
declare -a cpus=()

_get_cpus() {
	local state=${1:-online}

	_get_override_cpus || "_get_${state}_cpus"
}

_get_override_cpus() {
	((${#cpus_override[@]} > 0)) || return 1
	fold_list_onto_array cpus "${cpus_override[@]}"
}

_get_online_cpus() {
	fold_list_onto_array cpus $(get_online_cpus)
}

_get_offline_cpus() {
	fold_list_onto_array cpus $(get_offline_cpus)
}

get_all_irqs() {
	local irqs=() s_irqs=()

	irqs=(/proc/irq/+([0-9]))
	irqs=("${irqs[@]##*/}")

	# Sort it
	fold_list_onto_array s_irqs "${irqs[@]}"

	printf '%u\n' "${s_irqs[@]}"
}

check_this_irq() {
	local irq=${1:-0}

	((${#irqs_to_lookup[@]} > 0)) || return 0

	[[ -n ${irqs_to_lookup[irq]} ]]
}

update_irqs_sysfs() {
	local sysroot=${1:-}
	local input=$sysroot/sys/kernel/irq

	[[ -e $input ]] || return 1

	# per_cpu_count holds a list of all cpus, regardless of their state
	_get_cpus online
	_get_cpus offline

	local irq_path
	for irq_path in "$input/"*; do
		irq=${irq_path##*/}
		check_this_irq "$irq" || continue
		IFS="," read -ra cpu_counters < "$irq_path/per_cpu_count"
		for cpu in "${cpus[@]}"; do
			eval "_irq${irq}_cpu${cpu}+=(${cpu_counters[cpu]})"
			eval "_irq${irq}_counter[cpu]=_irq${irq}_cpu${cpu}[@]"
		done
		irqs_counters[irq]="_irq${irq}_counter[@]"
		irqs_devices[irq]=$(< "$irq_path/actions")
		irqs_types[irq]="$(< "$irq_path/chip_name") $(< "$irq_path/hwirq")-$(< "$irq_path/name")"
	done
}

update_irqs_procfs() {
	local input=${1:-/proc/interrupts}

	[[ -e $input ]] || return 1

	# /proc/interrupts shows only online CPUs. Use get_cpus() to get readings for
	# proper CPUs rather than parsing the actual header of the file.
	_get_cpus online

	local counter_idx
	while read -ra irqs; do
		irq=${irqs[0]%:*}
		[[ $irq == +([0-9]) ]] || continue
		check_this_irq "$irq" || continue
		cpu_counters=("${irqs[@]:1:${#cpus[@]}}") counter_idx=0
		for cpu in "${cpus[@]}"; do
			eval "_irq${irq}_cpu${cpu}+=(${cpu_counters[counter_idx++]})"
			eval "_irq${irq}_counter[cpu]=_irq${irq}_cpu${cpu}[@]"
		done
		irqs_counters[irq]="_irq${irq}_counter[@]"
		irqs_devices[irq]=${irqs[*]:${#cpus[@]}+1:2}
		irqs_types[irq]=${irqs[*]:${#cpus[@]}+3}
	done < "$input"
}

update_irqs() {
	local irqs irq
	local cpu cpu_counters=()
	local irqs_to_lookup=()

	fold_list_onto_array irqs_to_lookup "$@"

	update_irqs_sysfs || update_irqs_procfs
}

get_irqs() {
	local irqs=("$@") irq cpu
	local _counters counters counter delta total

	# If cpus[@] are not init, update was not run so nothing to check
	((${#cpus[@]} > 0)) || return 1

	if ((${#irqs[@]} == 0)); then
		irqs=($(get_all_irqs))
	fi

	for irq in "${irqs[@]}"; do
		for cpu in "${cpus[@]}"; do
			[[ -v "_irq${irq}_cpu${cpu}[@]" ]] || continue
			local -n counters="_irq${irq}_cpu${cpu}"
			# keep a separate copy to not touch the main _irq*[]
			_counters=("${counters[@]}") total=0

			if ((${#counters[@]} > 1)); then
				# Enhance output with calculating deltas between each reading
				for ((counter = 0; counter < ${#counters[@]} - 1; counter++)); do
					delta=$((counters[counter + 1] - counters[counter]))
					_counters[counter + 1]="${counters[counter + 1]} (+$delta)"
					: $((total += delta))
				done
			fi
			_counters+=("==$total")
			# Ignore idle irqs unless request from the env tells otherwise
			if [[ -n $SHOW_ALL_IRQS ]] || ((total > 0)); then
				echo "irq$irq->$(get_irq_type "$irq")->$(get_irq_device "$irq")@cpu$cpu:"
				printf '  %s\n' "${_counters[@]}"
			fi
		done
	done
}

get_irq_type() {
	[[ -n ${irqs_types[$1]} ]] || return 1
	echo "${irqs_types[$1]}"
}

get_irq_device() {
	[[ -n ${irqs_devices[$1]} ]] || return 1
	echo "${irqs_devices[$1]}"
}

reset_irqs() {
	irqs_counters=() irqs_devices=() irqs_types=() cpus=()

	unset -v "${!_irq@}"
}

read_irq_cpu_mask() {
	local irq=$1 mask=$2

	[[ -n $irq && -e /proc/irq/$irq || -n $mask ]] || return 1

	# smp_affinity holds a string of comma-separated 32-bit values. Iterate
	# over each dWORD and extract cpus bit by bit. Iterate from the end of
	# the array as that's where the first dWORD is located.
	local smp_affinity
	local bit dword dword_l=32 dword_idx
	local cpus=()

	if [[ -n $mask ]]; then
		IFS="," read -ra smp_affinity <<< "$mask"
	else
		IFS="," read -ra smp_affinity < "/proc/irq/$irq/smp_affinity"
	fi

	smp_affinity=("${smp_affinity[@]/#/0x}")

	for ((dword = ${#smp_affinity[@]} - 1, dword_idx = 0; dword >= 0; dword--, dword_idx++)); do
		bit=-1
		while ((++bit < dword_l)); do
			if ((smp_affinity[dword] & 1 << bit)); then
				cpus[bit + dword_l * dword_idx]=$bit
			fi
		done
	done

	printf '%u\n' "${!cpus[@]}"
}

read_irq_cpu_list() {
	local irq=$1 effective=${2:-0}

	[[ -n $irq && -e /proc/irq/$irq ]] || return 1

	if ((effective)); then
		parse_cpu_list "/proc/irq/$irq/effective_affinity_list"
	else
		parse_cpu_list "/proc/irq/$irq/smp_affinity_list"
	fi
}

build_irq_cpu_mask() {
	local cpu dword_l=32 dword_idx dword idxs
	local _mask=() mask=""

	for cpu; do
		dword_idx=$((cpu / dword_l))
		((_mask[dword_idx] |= 1 << (cpu - dword_l * dword_idx)))
	done

	# Store sorted list of dword indexes that we got
	idxs=("${!_mask[@]}")

	# Fill out all dWORDs starting from the highest (last) dword index
	for ((dword = idxs[-1]; dword >= 0; dword--)); do
		_mask[dword]=$(printf '%08x' "${_mask[dword]}")
		mask=${mask:+$mask,}${_mask[dword]}
	done

	echo "$mask"
}

squash_irq_cpu_mask() {
	local mask

	mask=$(build_irq_cpu_mask "$@")
	# E.g.: 1,32,64,65,77,88,127 -> 0x80000000010020030000000100000002
	# Valid under DPDK
	echo "0x${mask//,/}"
}

unsquash_irq_cpu_mask() {
	# E.g.: 0x80000000010020030000000100000002 -> 80000000,01002003,00000001,00000002
	# 8 is a max number of chars in a dWORD represented in hex.

	local smask=$1 _smask="" smask_l _smask_l

	smask=${smask/0x/} smask_l=$((${#smask} / 8)) _smask_l=$smask_l

	((smask_l == 0)) && echo "$smask" && return 0

	# Put comma at a right index
	while ((_smask_l)); do
		_smask+=,${smask:${#smask}-_smask_l--*8:8}
	done

	# Add remaining chars if any
	_smask=${smask::${#smask}-8*smask_l}${_smask}

	# If there were no chars left, drop the ',' from the beginning of the string
	echo "${_smask#,}"
}
