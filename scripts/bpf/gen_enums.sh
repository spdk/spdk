#!/usr/bin/env bash
set -e

rootdir=$(git rev-parse --show-toplevel)

_print_enums() {
	local enum_type=$1 enum_string=$2 enum_prefix=$3 enum output

	output=$(< "$rootdir/$(git -C "$rootdir" grep -G -l "$enum_string" -- lib module)")

	# Isolate the enum block
	output=${output#*$enum_string$'\n'} output=${output%%$'\n'\};*}
	# Fold it onto an array
	IFS="," read -ra output <<< "${output//[[:space:]]/}"
	# Drop the assignments
	output=("${output[@]/=*/}")

	for enum in "${!output[@]}"; do
		if [[ ${output[enum]} != "$enum_prefix"* ]]; then
			printf 'enum name %s does not start with expected prefix %s\n' "${output[enum]}" "$enum_prefix"
			return 1
		fi >&2
		printf '  @%s[%d] = "%s";\n' "$enum_type" "$enum" "${output[enum]#$enum_prefix}"
	done
}

print_enums() {
	for state in "${!state_enums[@]}"; do
		_print_enums "$state" "${state_enums["$state"]}" "${state_prefix["$state"]}"
	done
}

print_clear() { printf '  clear(@%s);\n' "${!state_enums[@]}"; }

declare -A state_enums=() state_prefix=()

state_enums["target"]="enum nvmf_tgt_state {"
state_enums["subsystem"]="enum spdk_nvmf_subsystem_state {"
state_prefix["target"]=NVMF_TGT_
state_prefix["subsystem"]=SPDK_NVMF_SUBSYSTEM_

enums=$(print_enums)
clear=$(print_clear)

cat <<- ENUM
	BEGIN {
		$enums
	}
	END {
		$clear
	}
ENUM
