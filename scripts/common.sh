#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2018 Intel Corporation
#  All rights reserved.
#

# Common shell utility functions

# Check if PCI device is in PCI_ALLOWED and not in PCI_BLOCKED
# Env:
# if PCI_ALLOWED is empty assume device is allowed
# if PCI_BLOCKED is empty assume device is NOT blocked
# Params:
# $1 - PCI BDF
function pci_can_use() {
	local i

	# The '\ ' part is important
	if [[ " $PCI_BLOCKED " =~ \ $1\  ]]; then
		return 1
	fi

	if [[ -z "$PCI_ALLOWED" ]]; then
		#no allow list specified, bind all devices
		return 0
	fi

	for i in $PCI_ALLOWED; do
		if [ "$i" == "$1" ]; then
			return 0
		fi
	done

	return 1
}

resolve_mod() {
	local mod=$1 aliases=()

	if aliases=($(modprobe -R "$mod")); then
		echo "${aliases[0]}"
	else
		echo "unknown"
	fi 2> /dev/null
}

cache_pci_init() {
	local -gA pci_bus_cache
	local -gA pci_ids_vendor
	local -gA pci_ids_device
	local -gA pci_bus_driver
	local -gA pci_mod_driver
	local -gA pci_mod_resolved
	local -gA pci_iommu_groups
	local -ga iommu_groups

	[[ -z ${pci_bus_cache[*]} || $CMD == reset ]] || return 1

	pci_bus_cache=()
	pci_bus_ids_vendor=()
	pci_bus_ids_device=()
	pci_bus_driver=()
	pci_mod_driver=()
	pci_mod_resolved=()
	pci_iommu_groups=()
	iommu_groups=()
}

cache_pci() {
	local pci=$1 class=$2 vendor=$3 device=$4 driver=$5 mod=$6

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
	if [[ -n $driver ]]; then
		pci_bus_driver["$pci"]=$driver
	fi
	if [[ -n $mod ]]; then
		pci_mod_driver["$pci"]=$mod
		pci_mod_resolved["$pci"]=$(resolve_mod "$mod")
	fi

	cache_pci_iommu_group "$pci"
}

cache_iommu_group() {
	local iommu_group=$1 pcis=() pci

	[[ -e /sys/kernel/iommu_groups/$iommu_group/type ]] || return 0

	local -n _iommu_group_ref=_iommu_group_$iommu_group

	iommu_groups[iommu_group]="_iommu_group_${iommu_group}[@]"

	for pci in "/sys/kernel/iommu_groups/$iommu_group/devices/"*; do
		pci=${pci##*/}
		[[ -n ${pci_iommu_groups["$pci"]} ]] && continue
		pci_iommu_groups["$pci"]=$iommu_group
		_iommu_group_ref+=("$pci")
	done

}

cache_pci_iommu_group() {
	local pci=$1 iommu_group

	[[ -e /sys/bus/pci/devices/$pci/iommu_group ]] || return 0

	iommu_group=$(readlink -f "/sys/bus/pci/devices/$pci/iommu_group")
	iommu_group=${iommu_group##*/}

	cache_iommu_group "$iommu_group"
}

is_iommu_enabled() {
	[[ -e /sys/kernel/iommu_groups/0 ]] && return 0
	[[ -e /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ]] || return 1
	[[ $(< /sys/module/vfio/parameters/enable_unsafe_noiommu_mode) == Y ]]
}

cache_pci_bus_sysfs() {
	[[ -e /sys/bus/pci/devices ]] || return 1

	cache_pci_init || return 0

	local pci
	local class vendor device driver mod

	for pci in /sys/bus/pci/devices/*; do
		class=$(< "$pci/class") vendor=$(< "$pci/vendor") device=$(< "$pci/device") driver="" mod=""
		driver=$(get_pci_driver_sysfs "${pci##*/}")
		if [[ -e $pci/modalias ]]; then
			mod=$(< "$pci/modalias")
		fi
		cache_pci "${pci##*/}" "$class" "$vendor" "$device" "$driver" "$mod"
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
		# pci class vendor device driver
		# lspci supports driver listing only under Linux, however, it's not
		# included when specific display mode (i.e. -mm) is in use, even if
		# extra -k is slapped on the cmdline. So with that in mind, just
		# get that info from sysfs.
		cache_pci "${dev[@]::4}" "$(get_pci_driver_sysfs "${dev[0]}")"
	done < <(lspci -Dnmm)
}

cache_pci_bus_pciconf() {
	hash pciconf 2> /dev/null || return 1

	cache_pci_init || return 0

	local class vendor device
	local pci pci_info
	local chip driver

	while read -r pci pci_info; do
		driver=${pci%@*}
		pci=${pci##*pci} pci=${pci%:}
		source <(echo "$pci_info")
		# pciconf under FreeBSD 13.1 provides vendor and device IDs in its
		# output under separate, dedicated fields. For 12.x they need to
		# be extracted from the chip field.
		if [[ -n $chip ]]; then
			vendor=$(printf '0x%04x' $((chip & 0xffff)))
			device=$(printf '0x%04x' $(((chip >> 16) & 0xffff)))
		fi
		cache_pci "$pci" "$class" "$vendor" "$device" "$driver"
	done < <(pciconf -l)
}

get_pci_driver_sysfs() {
	local pci=/sys/bus/pci/devices/$1 driver

	if [[ -e $pci/driver ]]; then
		driver=$(readlink -f "$pci/driver") driver=${driver##*/}
	fi
	echo "$driver"
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

# This function will ignore PCI PCI_ALLOWED and PCI_BLOCKED
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
		echo "${addr[0]}:${addr[1]}:${addr[2]}:${addr[3]}"
	elif iter_all_pci_sysfs "$(printf '0x%06x' $((0x$progif | 0x$subclass << 8 | 0x$class << 16)))"; then
		:
	else
		echo "Missing PCI enumeration utility" >&2
		exit 1
	fi
}

# This function will ignore PCI PCI_ALLOWED and PCI_BLOCKED
function iter_all_pci_dev_id() {
	local ven_id
	local dev_id
	ven_id="$(printf %04x $((0x$1)))"
	dev_id="$(printf %04x $((0x$2)))"

	if hash lspci &> /dev/null; then
		lspci -mm -n -D | awk -v ven="\"$ven_id\"" -v dev="\"${dev_id}\"" -F " " \
			'{if (ven ~ $3 && dev ~ $4) print $1}' | tr -d '"'
	elif hash pciconf &> /dev/null; then
		local addr=($(pciconf -l | grep -iE "chip=0x${dev_id}${ven_id}|vendor=0x$ven_id device=0x$dev_id" \
			| cut -d$'\t' -f1 | sed -e 's/^[a-zA-Z0-9_]*@pci//g' | tr ':' ' '))
		echo "${addr[0]}:${addr[1]}:${addr[2]}:${addr[3]}"
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

# This function will filter out PCI devices using PCI_ALLOWED and PCI_BLOCKED
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
	# then most likely PCI_ALLOWED option was used for setup.sh
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

block_in_use() {
	local block=$1 pt
	# Skip devices that are in use - simple blkid it to see if
	# there's any metadata (pt, fs, etc.) present on the drive.
	# FIXME: Special case to ignore atari as a potential false
	# positive:
	# https://github.com/spdk/spdk/issues/2079
	# Devices with SPDK's GPT part type are not considered to
	# be in use.

	if "$rootdir/scripts/spdk-gpt.py" "$block"; then
		return 1
	fi

	if ! pt=$(blkid -s PTTYPE -o value "/dev/${block##*/}"); then
		return 1
	elif [[ $pt == atari ]]; then
		return 1
	fi

	# Devices used in SPDK tests always create GPT partitions
	# with label containing SPDK_TEST string. Such devices were
	# part of the tests before, so are not considered in use.
	if [[ $pt == gpt ]] && parted "/dev/${block##*/}" -ms print | grep -q "SPDK_TEST"; then
		return 1
	fi

	return 0
}

get_spdk_gpt_old() {
	local spdk_guid

	[[ -e $rootdir/module/bdev/gpt/gpt.h ]] || return 1

	GPT_H="$rootdir/module/bdev/gpt/gpt.h"
	IFS="()" read -r _ spdk_guid _ < <(grep -w SPDK_GPT_PART_TYPE_GUID_OLD "$GPT_H")
	spdk_guid=${spdk_guid//, /-} spdk_guid=${spdk_guid//0x/}

	echo "$spdk_guid"
}

get_spdk_gpt() {
	local spdk_guid

	[[ -e $rootdir/module/bdev/gpt/gpt.h ]] || return 1

	GPT_H="$rootdir/module/bdev/gpt/gpt.h"
	IFS="()" read -r _ spdk_guid _ < <(grep -w SPDK_GPT_PART_TYPE_GUID "$GPT_H")
	spdk_guid=${spdk_guid//, /-} spdk_guid=${spdk_guid//0x/}

	echo "$spdk_guid"
}

map_supported_devices() {
	local ids dev_types dev_type dev_id bdf bdfs vmd _vmd

	local -gA nvme_d
	local -gA ioat_d dsa_d iaa_d
	local -gA virtio_d
	local -gA vmd_d nvme_vmd_d vmd_nvme_d vmd_nvme_count
	local -gA all_devices_d types_d all_devices_type_d

	ids+="PCI_DEVICE_ID_INTEL_IOAT" dev_types+="IOAT"
	ids+="|PCI_DEVICE_ID_INTEL_DSA" dev_types+="|DSA"
	ids+="|PCI_DEVICE_ID_INTEL_IAA" dev_types+="|IAA"
	ids+="|PCI_DEVICE_ID_VIRTIO" dev_types+="|VIRTIO"
	ids+="|PCI_DEVICE_ID_INTEL_VMD" dev_types+="|VMD"
	ids+="|SPDK_PCI_CLASS_NVME" dev_types+="|NVME"

	[[ -e $rootdir/include/spdk/pci_ids.h ]] || return 1

	((${#pci_bus_cache[@]} == 0)) && cache_pci_bus

	while read -r _ dev_type dev_id; do
		bdfs=(${pci_bus_cache["0x8086:$dev_id"]})
		[[ $dev_type == *NVME* ]] && bdfs=(${pci_bus_cache["$dev_id"]})
		[[ $dev_type == *VIRT* ]] && bdfs=(${pci_bus_cache["0x1af4:$dev_id"]})
		[[ $dev_type =~ ($dev_types) ]] && dev_type=${BASH_REMATCH[1],,}
		types_d["$dev_type"]=1
		for bdf in "${bdfs[@]}"; do
			eval "${dev_type}_d[$bdf]=0"
			all_devices_d["$bdf"]=0
			all_devices_type_d["$bdf"]=$dev_type
		done
	done < <(grep -E "$ids" "$rootdir/include/spdk/pci_ids.h")

	# Rebuild vmd refs from the very cratch to not have duplicates in case we were called
	# multiple times.
	unset -v "${!_vmd_@}"

	for bdf in "${!nvme_d[@]}"; do
		vmd=$(is_nvme_behind_vmd "$bdf") && _vmd=${vmd//[:.]/_} || continue
		nvme_vmd_d["$bdf"]=$vmd
		vmd_nvme_d["$vmd"]="_vmd_${_vmd}_nvmes[@]"
		((++vmd_nvme_count["$vmd"]))
		eval "_vmd_${_vmd}_nvmes+=($bdf)"
	done
}

is_nvme_behind_vmd() {
	local nvme_bdf=$1 dev_path

	IFS="/" read -ra dev_path < <(readlink -f "/sys/bus/pci/devices/$nvme_bdf")

	for dev in "${dev_path[@]}"; do
		[[ -n $dev && -n ${vmd_d["$dev"]} ]] && echo $dev && return 0
	done
	return 1
}

is_nvme_iommu_shared_with_vmd() {
	local nvme_bdf=$1 vmd

	# This use-case is quite specific to vfio-pci|iommu setup
	is_iommu_enabled || return 1

	[[ -n ${nvme_vmd_d["$nvme_bdf"]} ]] || return 1
	# nvme is behind VMD ...
	((pci_iommu_groups["$nvme_bdf"] == pci_iommu_groups["${nvme_vmd_d["$nvme_bdf"]}"])) || return 1
	# ... and it shares iommu_group with it
}

if [[ -e "$CONFIG_WPDK_DIR/bin/wpdk_common.sh" ]]; then
	# Adjust uname to report the operating system as WSL, Msys or Cygwin
	# and the kernel name as Windows. Define kill() to invoke the SIGTERM
	# handler before causing a hard stop with TerminateProcess.
	source "$CONFIG_WPDK_DIR/bin/wpdk_common.sh"
fi

# Make sure we have access to proper binaries installed in pkgdep/common.sh
if [[ -e /etc/opt/spdk-pkgdep/paths/export.sh ]]; then
	source /etc/opt/spdk-pkgdep/paths/export.sh
fi > /dev/null
