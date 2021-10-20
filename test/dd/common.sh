source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/scripts/common.sh"

clear_nvme() {
	local bdev=$1
	local nvme_ref=$2
	local size=${3:-0xffff}

	local bs=$((1024 << 10)) # 1M
	local count=$(((size / bs) + (size % bs ? 1 : 0)))

	"${DD_APP[@]}" \
		--if="/dev/zero" \
		--bs="$bs" \
		--ob="$bdev" \
		--count="$count" \
		--json <(gen_conf $nvme_ref)
}

trunc_files() {
	local f
	for f; do : > "$f"; done
}

gen_conf() {
	xtrace_disable

	local ref_name
	local method methods
	local param params
	local config

	# Pick references to all assoc arrays and build subsystem's config
	# around them. The assoc array should be the name of the rpc method
	# suffixed with unique _ID (ID may be any string). Default arrays
	# should be prefixed with _method string. The keys of the array
	# should store names of the method's parameters - proper quoting
	# of the values is done here. extra_subsystems[] can store extra
	# json configuration for different subsystems, other than bdev.

	if (($# == 0)) && [[ -z ${!method_@} ]]; then
		return 1
	fi

	methods=("${@:-${!method_@}}")
	local IFS=","

	for ref_name in "${methods[@]}"; do
		method=${ref_name#*method_} method=${method%_*} params=()

		# FIXME: centos7's Bash got trapped in 2011:
		# local -n ref=$ref_name -> local: -n: invalid option
		# HACK: it with eval and partial refs instead.
		eval "local refs=(\${!${ref_name}[@]})"
		local param_ref

		for param in "${refs[@]}"; do
			param_ref="${ref_name}[$param]"
			if [[ ${!param_ref} =~ ^([0-9]+|true|false|\{.*\})$ ]]; then
				params+=("\"$param\": ${!param_ref}")
			else
				params+=("\"$param\": \"${!param_ref}\"")
			fi
		done

		config+=("$(
			cat <<- JSON
				{
				  "params": {
				    ${params[*]}
				  },
				  "method": "$method"
				}
			JSON
		)")
	done

	jq . <<- JSON | tee /dev/stderr
		{
		  "subsystems": [
		    {
		      "subsystem": "bdev",
		      "config": [
		        ${config[*]},
			{
			  "method": "bdev_wait_for_examine"
			}
		      ]
		    }
		    ${extra_subsystems[*]:+,${extra_subsystems[*]}}
		  ]
		}
	JSON

	xtrace_restore
}

gen_bytes() {
	xtrace_disable

	local max=$1
	local bytes
	local byte
	local string
	shift

	bytes=({a..z} {0..9})
	if (($#)); then
		bytes=("$@")
	fi

	for ((byte = 0; byte < max; byte++)); do
		string+=${bytes[RANDOM % ${#bytes[@]}]}
	done
	printf '%b' "$string"

	xtrace_restore
}

get_native_nvme_bs() {
	# This is now needed since spdk_dd will reject all bs smaller than the
	# native bs of given nvme. We need to make sure all tests are using
	# bs >= native_bs. Use identify here so we don't have to switch nvmes
	# between user space and the kernel back and forth.
	local pci=$1 lbaf id

	mapfile -t id < <("$rootdir/build/examples/identify" -r trtype:pcie "traddr:$pci")

	# Get size of the current LBAF
	[[ ${id[*]} =~ "Current LBA Format:"\ *"LBA Format #"([0-9]+) ]]
	lbaf=${BASH_REMATCH[1]}
	[[ ${id[*]} =~ "LBA Format #$lbaf: Data Size:"\ *([0-9]+) ]]
	lbaf=${BASH_REMATCH[1]}

	echo "$lbaf"
}

check_liburing() {
	# Simply check if spdk_dd links to liburing. If yes, log that information.
	local lib so
	local -g liburing_in_use=0

	while read -r lib _ so _; do
		if [[ $lib == liburing.so.* ]]; then
			printf '* spdk_dd linked to liburing\n'
			# For sanity, check build config to see if liburing was requested.
			if [[ -e $rootdir/test/common/build_config.sh ]]; then
				source "$rootdir/test/common/build_config.sh"
			fi
			if [[ $CONFIG_URING != y ]]; then
				printf '* spdk_dd built with liburing, but no liburing support requested?\n'
			fi
			if [[ ! -e $so ]]; then
				printf '* %s is missing, aborting\n' "$lib"
				return 1
			fi
			export liburing_in_use=1
			return 0
		fi
	done < <(LD_TRACE_LOADED_OBJECTS=1 "${DD_APP[@]}") >&2
}

init_zram() {
	[[ -e /sys/class/zram-control ]] || modprobe zram num_devices=0
	return
}

create_zram_dev() {
	cat /sys/class/zram-control/hot_add
}

remove_zram_dev() {
	local id=$1

	[[ -e /sys/block/zram$id ]]

	echo 1 > "/sys/block/zram$id/reset"
	echo "$id" > "/sys/class/zram-control/hot_remove"
}

set_zram_dev() {
	local id=$1
	local size=${2:-64M}

	[[ -e /sys/block/zram$id ]]

	echo "$size" > "/sys/block/zram$id/disksize"
}
