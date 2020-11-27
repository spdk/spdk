source "$rootdir/test/common/autotest_common.sh"

setup() {
	if [[ $1 == output ]]; then
		"$rootdir/scripts/setup.sh" "${@:2}"
	else
		"$rootdir/scripts/setup.sh" "$@" &> /dev/null
	fi
}

get_meminfo() {
	xtrace_disable

	local get=$1
	local node=$2
	local var val
	local mem_f mem

	mem_f=/proc/meminfo
	if [[ -e /sys/devices/system/node/node$node/meminfo ]]; then
		mem_f=/sys/devices/system/node/node$node/meminfo
	fi
	mapfile -t mem < "$mem_f"
	mem=("${mem[@]#Node +([0-9]) }")

	while IFS=": " read -r var val _; do
		[[ $var == "$get" ]] || continue
		echo "$val" && return 0
	done < <(printf '%s\n' "${mem[@]}")
	return 1

	xtrace_restore
}
