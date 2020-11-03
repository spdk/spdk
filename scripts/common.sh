# Common shell utility functions

# Check if PCI device is on PCI_WHITELIST and not on PCI_BLACKLIST
# Env:
# if PCI_WHITELIST is empty assume device is whitelistened
# if PCI_BLACKLIST is empty assume device is NOT blacklistened
# Params:
# $1 - PCI BDF
function pci_can_use() {
	local i

	# The '\ ' part is important
	if [[ " $PCI_BLACKLIST " =~ \ $1\  ]]; then
		return 1
	fi

	if [[ -z "$PCI_WHITELIST" ]]; then
		#no whitelist specified, bind all devices
		return 0
	fi

	for i in $PCI_WHITELIST; do
		if [ "$i" == "$1" ]; then
			return 0
		fi
	done

	return 1
}

cache_pci_init() {
	local -gA pci_bus_cache
	local -gA pci_ids_vendor
	local -gA pci_ids_device

	[[ -z ${pci_bus_cache[*]} || $CMD == reset ]] || return 1

	pci_bus_cache=()
	pci_bus_ids_vendor=()
	pci_bus_ids_device=()
}

cache_pci() {
	local pci=$1 class=$2 vendor=$3 device=$4

	if [[ -n $class ]]; then
		class=0x${class/0x/}
		pci_bus_cache["$class"]="${pci_bus_cache["$class"]:+${pci_bus_cache["$class"]} }$pci"
	fi
	if [[ -n $vendor && -n $device ]]; then
		vendor=0x${vendor/0x/} device=0x${device/0x/}
		pci_bus_cache["$vendor:$device"]="${pci_bus_cache["$vendor:$device"]:+${pci_bus_cache["$vendor:$device"]} }$pci"

		pci_ids_vendor["$pci"]=$vendor
		pci_ids_device["$pci"]=$device
	fi
}

cache_pci_bus_sysfs() {
	[[ -e /sys/bus/pci/devices ]] || return 1

	cache_pci_init || return 0

	local pci
	local class vendor device

	for pci in /sys/bus/pci/devices/*; do
		class=$(< "$pci/class") vendor=$(< "$pci/vendor") device=$(< "$pci/device")
		cache_pci "${pci##*/}" "$class" "$vendor" "$device"
	done
}

cache_pci_bus_lspci() {
	hash lspci 2> /dev/null || return 1

	cache_pci_init || return 0

	local dev
	while read -ra dev; do
		dev=("${dev[@]//\"/}")
		# lspci splits ls byte of the class (prog. interface) into a separate
		# field if it's != 0. Look for it and normalize the value to fit with
		# what kernel exposes under sysfs.
		if [[ ${dev[*]} =~ -p([0-9]+) ]]; then
			dev[1]+=${BASH_REMATCH[1]}
		else
			dev[1]+=00
		fi
		# pci class vendor device
		cache_pci "${dev[@]::4}"
	done < <(lspci -Dnmm)
}

cache_pci_bus_pciconf() {
	hash pciconf 2> /dev/null || return 1

	cache_pci_init || return 0

	local class vd vendor device
	local pci domain bus device function

	while read -r pci class _ vd _; do
		IFS=":" read -r domain bus device function _ <<< "${pci##*pci}"
		pci=$(printf '%04x:%02x:%02x:%x' \
			"$domain" "$bus" "$device" "$function")
		class=$(printf '0x%06x' $((class)))
		vendor=$(printf '0x%04x' $((vd & 0xffff)))
		device=$(printf '0x%04x' $(((vd >> 16) & 0xffff)))

		cache_pci "$pci" "$class" "$vendor" "$device"
	done < <(pciconf -l)
}

cache_pci_bus() {
	case "$(uname -s)" in
		Linux) cache_pci_bus_lspci || cache_pci_bus_sysfs ;;
		FreeBSD) cache_pci_bus_pciconf ;;
	esac
}

iter_all_pci_sysfs() {
	cache_pci_bus_sysfs || return 1

	# default to class of the nvme devices
	local find=${1:-0x010802} findx=$2
	local pci pcis

	[[ -n ${pci_bus_cache["$find"]} ]] || return 0
	read -ra pcis <<< "${pci_bus_cache["$find"]}"

	if ((findx)); then
		printf '%s\n' "${pcis[@]::findx}"
	else
		printf '%s\n' "${pcis[@]}"
	fi
}

# This function will ignore PCI PCI_WHITELIST and PCI_BLACKLIST
function iter_all_pci_class_code() {
	local class
	local subclass
	local progif
	class="$(printf %02x $((0x$1)))"
	subclass="$(printf %02x $((0x$2)))"
	progif="$(printf %02x $((0x$3)))"

	if hash lspci &> /dev/null; then
		if [ "$progif" != "00" ]; then
			lspci -mm -n -D \
				| grep -i -- "-p${progif}" \
				| awk -v cc="\"${class}${subclass}\"" -F " " \
					'{if (cc ~ $2) print $1}' | tr -d '"'
		else
			lspci -mm -n -D \
				| awk -v cc="\"${class}${subclass}\"" -F " " \
					'{if (cc ~ $2) print $1}' | tr -d '"'
		fi
	elif hash pciconf &> /dev/null; then
		local addr=($(pciconf -l | grep -i "class=0x${class}${subclass}${progif}" \
			| cut -d$'\t' -f1 | sed -e 's/^[a-zA-Z0-9_]*@pci//g' | tr ':' ' '))
		printf "%04x:%02x:%02x:%x\n" ${addr[0]} ${addr[1]} ${addr[2]} ${addr[3]}
	elif iter_all_pci_sysfs "$(printf '0x%06x' $((0x$progif | 0x$subclass << 8 | 0x$class << 16)))"; then
		:
	else
		echo "Missing PCI enumeration utility" >&2
		exit 1
	fi
}

# This function will ignore PCI PCI_WHITELIST and PCI_BLACKLIST
function iter_all_pci_dev_id() {
	local ven_id
	local dev_id
	ven_id="$(printf %04x $((0x$1)))"
	dev_id="$(printf %04x $((0x$2)))"

	if hash lspci &> /dev/null; then
		lspci -mm -n -D | awk -v ven="\"$ven_id\"" -v dev="\"${dev_id}\"" -F " " \
			'{if (ven ~ $3 && dev ~ $4) print $1}' | tr -d '"'
	elif hash pciconf &> /dev/null; then
		local addr=($(pciconf -l | grep -i "chip=0x${dev_id}${ven_id}" \
			| cut -d$'\t' -f1 | sed -e 's/^[a-zA-Z0-9_]*@pci//g' | tr ':' ' '))
		printf "%04x:%02x:%02x:%x\n" ${addr[0]} ${addr[1]} ${addr[2]} ${addr[3]}
	elif iter_all_pci_sysfs "0x$ven_id:0x$dev_id"; then
		:
	else
		echo "Missing PCI enumeration utility" >&2
		exit 1
	fi
}

function iter_pci_dev_id() {
	local bdf=""

	for bdf in $(iter_all_pci_dev_id "$@"); do
		if pci_can_use "$bdf"; then
			echo "$bdf"
		fi
	done
}

# This function will filter out PCI devices using PCI_WHITELIST and PCI_BLACKLIST
# See function pci_can_use()
function iter_pci_class_code() {
	local bdf=""

	for bdf in $(iter_all_pci_class_code "$@"); do
		if pci_can_use "$bdf"; then
			echo "$bdf"
		fi
	done
}

function nvme_in_userspace() {
	# Check used drivers. If it's not vfio-pci or uio-pci-generic
	# then most likely PCI_WHITELIST option was used for setup.sh
	# and we do not want to use that disk.

	local bdf bdfs
	local nvmes

	if [[ -n ${pci_bus_cache["0x010802"]} ]]; then
		nvmes=(${pci_bus_cache["0x010802"]})
	else
		nvmes=($(iter_pci_class_code 01 08 02))
	fi

	for bdf in "${nvmes[@]}"; do
		if [[ -e /sys/bus/pci/drivers/nvme/$bdf ]] \
			|| [[ $(uname -s) == FreeBSD && $(pciconf -l "pci${bdf/./:}") == nvme* ]]; then
			continue
		fi
		bdfs+=("$bdf")
	done
	((${#bdfs[@]})) || return 1
	printf '%s\n' "${bdfs[@]}"
}

cmp_versions() {
	local ver1 ver1_l
	local ver2 ver2_l

	IFS=".-:" read -ra ver1 <<< "$1"
	IFS=".-:" read -ra ver2 <<< "$3"
	local op=$2

	ver1_l=${#ver1[@]}
	ver2_l=${#ver2[@]}

	local lt=0 gt=0 eq=0 v
	case "$op" in
		"<") : $((eq = gt = 1)) ;;
		">") : $((eq = lt = 1)) ;;
		"<=") : $((gt = 1)) ;;
		">=") : $((lt = 1)) ;;
		"==") : $((lt = gt = 1)) ;;
	esac

	decimal() (
		local d=${1,,}
		if [[ $d =~ ^[0-9]+$ ]]; then
			echo $((10#$d))
		elif [[ $d =~ ^0x || $d =~ ^[a-f0-9]+$ ]]; then
			d=${d/0x/}
			echo $((0x$d))
		else
			echo 0
		fi
	)

	for ((v = 0; v < (ver1_l > ver2_l ? ver1_l : ver2_l); v++)); do
		ver1[v]=$(decimal "${ver1[v]}")
		ver2[v]=$(decimal "${ver2[v]}")
		((ver1[v] > ver2[v])) && return "$gt"
		((ver1[v] < ver2[v])) && return "$lt"
	done
	[[ ${ver1[*]} == "${ver2[*]}" ]] && return "$eq"
}

lt() { cmp_versions "$1" "<" "$2"; }
gt() { cmp_versions "$1" ">" "$2"; }
le() { cmp_versions "$1" "<=" "$2"; }
ge() { cmp_versions "$1" ">=" "$2"; }
eq() { cmp_versions "$1" "==" "$2"; }
neq() { ! eq "$1" "$2"; }
