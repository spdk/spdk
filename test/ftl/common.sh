#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#

# Common utility functions to be sourced by the libftl test scripts

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../..")
rpc_py=$rootdir/scripts/rpc.py

export ftl_tgt_core_mask="[0]"

export spdk_tgt_bin="$SPDK_BIN_DIR/spdk_tgt"
export spdk_tgt_cpumask="[0]"
export spdk_tgt_cnfg="${testdir}/config/tgt.json"
export spdk_tgt_pid=""

export spdk_ini_bin="$SPDK_BIN_DIR/spdk_tgt"
export spdk_ini_cpumask="[1]"
export spdk_ini_rpc="/var/tmp/spdk.tgt.sock"
export spdk_ini_cnfg="${testdir}/config/ini.json"
export spdk_ini_pid=""

export spdk_dd_bin="$SPDK_BIN_DIR/spdk_dd"

function clear_lvols() {
	stores=$($rpc_py bdev_lvol_get_lvstores | jq -r ".[] | .uuid")
	for lvs in $stores; do
		$rpc_py bdev_lvol_delete_lvstore -u $lvs
	done
}

function create_nv_cache_bdev() {
	local name=$1
	local cache_bdf=$2
	local base_bdev=$3
	local cache_size=$4

	# use 5% space of base bdev
	local base_size=$(($(get_bdev_size "$base_bdev") * 5 / 100))

	# Create NVMe bdev on specified device and split it so that it has the desired size
	local nvc_bdev
	nvc_bdev=$($rpc_py bdev_nvme_attach_controller -b "$name" -t PCIe -a "$cache_bdf")

	if [[ -z "$cache_size" ]]; then
		cache_size=$(($(get_bdev_size "$base_bdev") * 5 / 100))
	fi
	$rpc_py bdev_split_create "$nvc_bdev" -s "$cache_size" 1
}

function create_base_bdev() {
	local name=$1
	local base_bdf=$2
	local size=$3

	# Create NVMe bdev on specified device and split it so that it has the desired size
	local base_bdev
	base_bdev=$($rpc_py bdev_nvme_attach_controller -b $name -t PCIe -a $base_bdf)

	local base_size
	base_size=$(get_bdev_size $base_bdev)
	if [[ $size -le $base_size ]]; then
		$rpc_py bdev_split_create $base_bdev -s $size 1
	else
		clear_lvols
		lvs=$($rpc_py bdev_lvol_create_lvstore $base_bdev lvs)
		$rpc_py bdev_lvol_create ${base_bdev}p0 $size -t -u $lvs
	fi
}

# required input variables:
# FTL_BDEV - FTL bdev name
# FTL_BASE - FTL base device PCIe path
# FTL_BASE_SIZE - FTL base device size
# FTL_CACHE - FTL NV cache device PCIe path
# FTL_CACHE_SIZE - FTL NV cache device size
# FTL_L2P_DRAM_LIMIT - FTL L2P DRAM limit
function tcp_target_setup() {
	local base_bdev=""
	local cache_bdev=""

	if [[ -f "$spdk_tgt_cnfg" ]]; then
		$spdk_tgt_bin "--cpumask=$spdk_tgt_cpumask" --config="$spdk_tgt_cnfg" &
	else
		$spdk_tgt_bin "--cpumask=$spdk_tgt_cpumask" &
	fi
	spdk_tgt_pid=$!
	export spdk_tgt_pid
	waitforlisten "$spdk_tgt_pid"

	if [[ -f "$spdk_tgt_cnfg" ]]; then
		# Configuration loaded from the JSON file
		return 0
	fi

	# Check if input parameters are available
	local params=(FTL_BDEV FTL_BASE FTL_BASE_SIZE FTL_CACHE FTL_CACHE_SIZE FTL_L2P_DRAM_LIMIT)
	for param in "${params[@]}"; do
		if [[ -z "${!param}" ]]; then
			echo "Missing $param"
			exit 1
		fi
	done

	base_bdev=$(create_base_bdev base "$FTL_BASE" "$FTL_BASE_SIZE")
	if [[ -z "$base_bdev" ]]; then
		echo "Cannot create base device"
		exit 1
	fi

	cache_bdev=$(create_nv_cache_bdev cache "$FTL_CACHE" "$base_bdev" "$FTL_CACHE_SIZE")
	if [[ -z "$cache_bdev" ]]; then
		echo "Cannot create nv cache device"
		exit 1
	fi

	$rpc_py -t 60 bdev_ftl_create -b "$FTL_BDEV" -d "$base_bdev" -c "$cache_bdev" --l2p_dram_limit "$FTL_L2P_DRAM_LIMIT"

	$rpc_py nvmf_create_transport --trtype TCP
	$rpc_py nvmf_create_subsystem nqn.2018-09.io.spdk:cnode0 -a -m 1
	$rpc_py nvmf_subsystem_add_ns nqn.2018-09.io.spdk:cnode0 "$FTL_BDEV"
	$rpc_py nvmf_subsystem_add_listener nqn.2018-09.io.spdk:cnode0 -t TCP -f ipv4 -s 4420 -a 127.0.0.1

	$rpc_py save_config > "$spdk_tgt_cnfg"
}

function tcp_target_shutdown() {
	if [[ -n "$spdk_tgt_pid" ]]; then
		killprocess "$spdk_tgt_pid"
		unset spdk_tgt_pid
	fi
}

function tcp_target_shutdown_dirty() {
	if [[ -n "$spdk_tgt_pid" ]]; then
		kill -9 "$spdk_tgt_pid"
		unset spdk_tgt_pid
	fi
}

function tcp_target_cleanup() {
	tcp_target_shutdown
	rm -f "$spdk_tgt_cnfg"
}

# required input variables:
# FTL_BDEV - FTL bdev name
function tcp_initiator_setup() {
	local rpc="$rpc_py -s ${spdk_ini_rpc}"

	if [[ -f "$spdk_ini_cnfg" ]]; then
		return 0
	fi

	if [[ -z "$FTL_BDEV" ]]; then
		echo "Missing FTL_BDEV"
		exit 1
	fi

	$spdk_ini_bin --cpumask="$spdk_ini_cpumask" --rpc-socket="$spdk_ini_rpc" &
	spdk_ini_pid=$!
	export spdk_ini_pid
	waitforlisten $spdk_ini_pid $spdk_ini_rpc

	$rpc bdev_nvme_attach_controller -b "$FTL_BDEV" \
		-t tcp -a 127.0.0.1 -s 4420 -f ipv4 -n nqn.2018-09.io.spdk:cnode0

	(
		echo '{"subsystems": ['
		$rpc save_subsystem_config -n bdev
		echo ']}'
	) > "$spdk_ini_cnfg"

	killprocess $spdk_ini_pid
	unset spdk_ini_pid
}

function tcp_initiator_shutdown() {
	if [[ -n "$spdk_ini_pid" ]]; then
		killprocess "$spdk_ini_pid"
		unset spdk_ini_pid
	fi
}

function tcp_initiator_cleanup() {
	tcp_initiator_shutdown
	rm -f "$spdk_ini_cnfg"
}

function tcp_cleanup() {
	tcp_target_cleanup
	tcp_initiator_cleanup
}

function tcp_dd() {
	tcp_initiator_setup
	$spdk_dd_bin --cpumask="$spdk_ini_cpumask" --rpc-socket="$spdk_ini_rpc" --json="$spdk_ini_cnfg" "$@"
}

# Remove not needed files from shared memory
function remove_shm() {
	echo Remove shared memory files
	rm -f rm -f /dev/shm/ftl*
	rm -f rm -f /dev/hugepages/ftl*
	rm -f rm -f /dev/shm/spdk*
	rm -f rm -f /dev/shm/iscsi
	rm -f rm -f /dev/hugepages/spdk*
}
