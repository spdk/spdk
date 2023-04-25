#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2023 Intel Corporation
#  All rights reserved.
#

source "$rootdir/test/common/autotest_common.sh"

shopt -s extglob

NVME_CMD="/usr/local/src/nvme-cli/nvme"

rpc_py=$rootdir/scripts/rpc.py

declare -A ctrls=()
declare -A nvmes=()
declare -A bdfs=()
nvme_name=""

nvme_get() {
	local ref=$1 reg val
	shift

	local -gA "$ref=()"
	while IFS=":" read -r reg val; do
		[[ -n $val ]] \
			&& eval "${ref}[${reg//[[:space:]]/}]=\"${val##+([[:space:]])}\""
	done < <($NVME_CMD "$@")
}

scan_nvme_ctrls() {
	# Create set of references and bundle them together in global assoc arrays.
	# Each controller's regs are mapped onto a separate array named as the target
	# ctrl. Each ctrl also gets a dedicated array which holds references to each
	# namespace device. E.g.:
	#
	#  ctrls["nvme0"]=nvme0
	#    nvme0["mn"] nvme0["vid"]
	#  nvmes["nvme0"]=nvme0_ns
	#    nvme0_ns[1]=nvme0n1 nvme0_ns[2]=nvme0n2
	#     nvme0n1["lbaf0"] nvme0n1["ncap"]
	#  bdf["nvme0"]=0000:00:42.0
	#
	# Each of these can be accessed via get*() helpers defined below. E.g.:
	# get_nvme_ctrl_feature nvme0 vid -> 0x8086
	# get_nvme_ns_feature nvme0 1 ncap -> 0x2e9390b0
	# get_nvme_nss nvme0 -> 1 2

	local ctrl ctrl_dev reg val ns pci

	for ctrl in /sys/class/nvme/nvme*; do
		[[ -e $ctrl ]] || continue
		pci=$(< "$ctrl/address")
		pci_can_use "$pci" || continue
		ctrl_dev=${ctrl##*/}
		nvme_get "$ctrl_dev" id-ctrl "/dev/$ctrl_dev"
		local -n _ctrl_ns=${ctrl_dev}_ns
		for ns in "$ctrl/${ctrl##*/}n"*; do
			[[ -e $ns ]] || continue
			ns_dev=${ns##*/}
			nvme_get "$ns_dev" id-ns "/dev/$ns_dev"
			_ctrl_ns[${ns##*n}]=$ns_dev
		done
		ctrls["$ctrl_dev"]=$ctrl_dev
		nvmes["$ctrl_dev"]=${ctrl_dev}_ns
		bdfs["$ctrl_dev"]=$pci
	done
}

get_nvme_ctrl_feature() {
	local ctrl=$1 reg=${2:-cntlid}

	[[ -n ${ctrls["$ctrl"]} ]] || return 1

	local -n _ctrl=${ctrls["$ctrl"]}

	[[ -n ${_ctrl["$reg"]} ]] || return 1
	echo "${_ctrl["$reg"]}"
}

get_nvme_ns_feature() {
	local ctrl=$1 ns=$2 reg=${3:-nsze}

	[[ -n ${nvmes["$ctrl"]} ]] || return 1

	local -n _nss=${nvmes["$ctrl"]}
	[[ -n ${_nss[ns]} ]] || return 1

	local -n _ns=${_nss[ns]}

	[[ -n ${_ns["$reg"]} ]] || return 1
	echo "${_ns["$reg"]}"
}

get_nvme_nss() {
	local ctrl=$1

	[[ -n ${nvmes["$ctrl"]} ]] || return 1
	local -n _nss=${nvmes["$ctrl"]}

	echo "${!_nss[@]}"
}

get_active_lbaf() {
	local ctrl=$1 ns=$2 reg lbaf

	[[ -n ${nvmes["$ctrl"]} ]] || return 1

	local -n _nss=${nvmes["$ctrl"]}
	[[ -n ${_nss[ns]} ]] || return 1

	local -n _ns=${_nss[ns]}

	for reg in "${!_ns[@]}"; do
		[[ $reg == lbaf* ]] || continue
		[[ ${_ns["$reg"]} == *"in use"* ]] || continue
		echo "${reg/lbaf/}" && return 0
	done
	return 1
}

get_oacs() {
	local ctrl=${1:-nvme0} bit=${2:-nsmgt}
	local -A bits

	# Figure 275: Identify – Identify Controller Data Structure, I/O Command Set Independent
	bits["ss/sr"]=$((1 << 0))
	bits["fnvme"]=$((1 << 1))
	bits["fc/fi"]=$((1 << 2))
	bits["nsmgt"]=$((1 << 3))
	bits["self-test"]=$((1 << 4))
	bits["directives"]=$((1 << 5))
	bits["nvme-mi-s/r"]=$((1 << 6))
	bits["virtmgt"]=$((1 << 7))
	bits["doorbellbuf"]=$((1 << 8))
	bits["getlba"]=$((1 << 9))
	bits["commfeatlock"]=$((1 << 10))

	bit=${bit,,}
	[[ -n ${bits["$bit"]} ]] || return 1

	(($(get_nvme_ctrl_feature "$ctrl" oacs) & bits["$bit"]))
}

get_nvmes_with_ns_management() {
	((${#ctrls[@]} == 0)) && scan_nvme_ctrls

	local ctrl
	for ctrl in "${!ctrls[@]}"; do
		get_oacs "$ctrl" nsmgt && echo "$ctrl"
	done
}

get_nvme_with_ns_management() {
	local _ctrls

	_ctrls=($(get_nvmes_with_ns_management))
	if ((${#_ctrls[@]} > 0)); then
		echo "${_ctrls[0]}"
		return 0
	fi
	return 1
}

get_ctratt() {
	local ctrl=$1
	get_nvme_ctrl_feature "$ctrl" ctratt
}

ctrl_has_fdp() {
	local ctrl=$1 ctratt

	ctratt=$(get_ctratt "$ctrl")
	# See include/spdk/nvme_spec.h
	((ctratt & 1 << 19))
}

get_ctrls_with_fdp() {
	((${#ctrls[@]} == 0)) && scan_nvme_ctrls

	local ctrl
	for ctrl in "${!ctrls[@]}"; do
		ctrl_has_fdp "$ctrl" && echo "$ctrl"
	done

}

get_ctrl_with_fdp() {
	local _ctrls

	_ctrls=($(get_ctrls_with_fdp))
	if ((${#_ctrls[@]} > 0)); then
		echo "${_ctrls[0]}"
		return 0
	fi
	return 1
}
