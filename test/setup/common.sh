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

partition_drive() {
	local disk=$1
	local part_no=${2:-2}
	local size=${3:-1073741824} # default 1G

	local part part_start=0 part_end=0
	local parts=()

	for ((part = 1; part <= part_no; part++)); do
		parts+=("${disk}p$part")
	done

	# Convert size to sectors for more precise partitioning
	((size /= $(< "/sys/class/block/$disk/queue/physical_block_size")))

	"$rootdir/scripts/sync_dev_uevents.sh" block/partition "${parts[@]}" &

	# Avoid parted since it generates to much noise over netlink
	sgdisk "/dev/$disk" --zap-all || :
	for ((part = 1; part <= part_no; part++)); do
		((part_start = part_start == 0 ? 2048 : part_end + 1))
		((part_end = part_start + size - 1))
		sgdisk "/dev/$disk" --new="$part:$part_start:$part_end"
	done
	wait "$!"
}

mkfs() {
	local dev=$1 mount=$2 size=$3

	mkdir -p "$mount"

	[[ -e $dev ]]
	mkfs.ext4 -qF "$dev" $size
	mount "$dev" "$mount"
}

sec_size_to_bytes() {
	local dev=$1

	[[ -e /sys/block/$dev ]] || return 1
	# /size is always represented in 512B blocks
	echo $(($(< "/sys/block/$dev/size") * 512))
}
