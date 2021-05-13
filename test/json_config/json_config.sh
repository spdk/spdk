#!/usr/bin/env bash

rootdir=$(readlink -f $(dirname $0)/../..)
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

if [[ $SPDK_TEST_ISCSI -eq 1 ]]; then
	source "$rootdir/test/iscsi_tgt/common.sh"
fi

if [[ $SPDK_TEST_VHOST -ne 1 && $SPDK_TEST_VHOST_INIT -eq 1 ]]; then
	SPDK_TEST_VHOST=1
	echo "WARNING: Virtio initiator JSON_config test requires vhost target."
	echo "         Setting SPDK_TEST_VHOST=1 for duration of current script."
fi

if ((SPDK_TEST_BLOCKDEV + \
	SPDK_TEST_ISCSI + \
	SPDK_TEST_NVMF + \
	SPDK_TEST_VHOST + \
	SPDK_TEST_VHOST_INIT + \
	SPDK_TEST_PMDK + \
	SPDK_TEST_RBD == 0)); then
	echo "WARNING: No tests are enabled so not running JSON configuration tests"
	exit 0
fi

declare -A app_pid=([target]="" [initiator]="")
declare -A app_socket=([target]='/var/tmp/spdk_tgt.sock' [initiator]='/var/tmp/spdk_initiator.sock')
declare -A app_params=([target]='-m 0x1 -s 1024' [initiator]='-m 0x2 -g -u -s 1024')
declare -A configs_path=([target]="$rootdir/spdk_tgt_config.json" [initiator]="$rootdir/spdk_initiator_config.json")

function tgt_rpc() {
	$rootdir/scripts/rpc.py -s "${app_socket[target]}" "$@"
}

function initiator_rpc() {
	$rootdir/scripts/rpc.py -s "${app_socket[initiator]}" "$@"
}

last_event_id=0

function tgt_check_notification_types() {
	timing_enter "${FUNCNAME[0]}"

	local ret=0
	local enabled_types=("bdev_register" "bdev_unregister")

	local get_types=($(tgt_rpc notify_get_types | jq -r '.[]'))
	if [[ ${enabled_types[*]} != "${get_types[*]}" ]]; then
		echo "ERROR: expected types: ${enabled_types[*]}, but got: ${get_types[*]}"
		ret=1
	fi

	timing_exit "${FUNCNAME[0]}"
	return $ret
}

get_notifications() {
	local ev_type ev_ctx event_id

	while IFS=":" read -r ev_type ev_ctx event_id; do
		echo "$ev_type:$ev_ctx"
	done < <(tgt_rpc notify_get_notifications -i "$last_event_id" | jq -r '.[] | "\(.type):\(.ctx):\(.id)"')
}

function tgt_check_notifications() {
	local events_to_check
	local recorded_events

	events_to_check=("$@")
	recorded_events=($(get_notifications))

	# These should be in order hence compare entire arrays
	if [[ ${events_to_check[*]} != "${recorded_events[*]}" ]]; then
		cat <<- ERROR
			Expected events did not match.

			Expected:
			$(printf ' %s\n' "${events_to_check[@]}")
			Recorded:
			$(printf ' %s\n' "${recorded_events[@]}")
		ERROR
		return 1
	fi

	cat <<- INFO
		Expected events matched:
		$(printf ' %s\n' "${recorded_events[@]}")
	INFO
}

# $1 - target / initiator
# $2..$n app parameters
function json_config_test_start_app() {
	local app=$1
	shift

	[[ -n "${#app_socket[$app]}" ]] # Check app type
	[[ -z "${app_pid[$app]}" ]]     # Assert if app is not running

	local app_extra_params=""
	if [[ $SPDK_TEST_VHOST -eq 1 || $SPDK_TEST_VHOST_INIT -eq 1 ]]; then
		# If PWD is nfs/sshfs we can't create UNIX sockets there. Always use safe location instead.
		app_extra_params='-S /var/tmp'
	fi

	$SPDK_BIN_DIR/spdk_tgt ${app_params[$app]} ${app_extra_params} -r ${app_socket[$app]} "$@" &
	app_pid[$app]=$!

	echo "Waiting for $app to run..."
	waitforlisten ${app_pid[$app]} ${app_socket[$app]}
	echo ""
}

# $1 - target / initiator
function json_config_test_shutdown_app() {
	local app=$1

	# Check app type && assert app was started
	[[ -n "${#app_socket[$app]}" ]]
	[[ -n "${app_pid[$app]}" ]]

	# spdk_kill_instance RPC will trigger ASAN
	kill -SIGINT ${app_pid[$app]}

	for ((i = 0; i < 30; i++)); do
		if ! kill -0 ${app_pid[$app]} 2> /dev/null; then
			app_pid[$app]=
			break
		fi
		sleep 0.5
	done

	if [[ -n "${app_pid[$app]}" ]]; then
		echo "SPDK $app shutdown timeout"
		return 1
	fi

	echo "SPDK $app shutdown done"
}

function create_bdev_subsystem_config() {
	timing_enter "${FUNCNAME[0]}"

	local expected_notifications=()

	# Consider multiple nvme devices loaded into the subsystem prior running
	# the tests.
	expected_notifications+=($(get_notifications))

	if [[ $SPDK_TEST_BLOCKDEV -eq 1 ]]; then
		local lvol_store_base_bdev=Nvme0n1

		tgt_rpc bdev_split_create $lvol_store_base_bdev 2
		tgt_rpc bdev_split_create Malloc0 3
		tgt_rpc bdev_malloc_create 8 4096 --name Malloc3
		tgt_rpc bdev_passthru_create -b Malloc3 -p PTBdevFromMalloc3

		tgt_rpc bdev_null_create Null0 32 512

		tgt_rpc bdev_malloc_create 32 512 --name Malloc0
		tgt_rpc bdev_malloc_create 16 4096 --name Malloc1

		expected_notifications+=(
			bdev_register:${lvol_store_base_bdev}p1
			bdev_register:${lvol_store_base_bdev}p0
			bdev_register:Malloc3
			bdev_register:PTBdevFromMalloc3
			bdev_register:Null0
			bdev_register:Malloc0
			bdev_register:Malloc0p2
			bdev_register:Malloc0p1
			bdev_register:Malloc0p0
			bdev_register:Malloc1
		)

		if [[ $(uname -s) = Linux ]]; then
			# This AIO bdev must be large enough to be used as LVOL store
			dd if=/dev/zero of="$SPDK_TEST_STORAGE/sample_aio" bs=1024 count=102400
			tgt_rpc bdev_aio_create "$SPDK_TEST_STORAGE/sample_aio" aio_disk 1024
			expected_notifications+=(bdev_register:aio_disk)
		fi

		# For LVOLs use split to check for proper order of initialization.
		# If LVOLs configuration will be reordered (eg moved before splits or AIO/NVMe)
		# it should fail loading JSON config from file.
		tgt_rpc bdev_lvol_create_lvstore -c 1048576 ${lvol_store_base_bdev}p0 lvs_test

		expected_notifications+=(
			"bdev_register:$(tgt_rpc bdev_lvol_create -l lvs_test lvol0 32)"
			"bdev_register:$(tgt_rpc bdev_lvol_create -l lvs_test -t lvol1 32)"
			"bdev_register:$(tgt_rpc bdev_lvol_snapshot lvs_test/lvol0 snapshot0)"
			"bdev_register:$(tgt_rpc bdev_lvol_clone lvs_test/snapshot0 clone0)"
		)
	fi

	if [[ $SPDK_TEST_CRYPTO -eq 1 ]]; then
		tgt_rpc bdev_malloc_create 8 1024 --name MallocForCryptoBdev
		if [[ $(lspci -d:37c8 | wc -l) -eq 0 ]]; then
			local crypto_driver=crypto_aesni_mb
		else
			local crypto_driver=crypto_qat
		fi

		tgt_rpc bdev_crypto_create MallocForCryptoBdev CryptoMallocBdev $crypto_driver 0123456789123456
		expected_notifications+=(
			bdev_register:MallocForCryptoBdev
			bdev_register:CryptoMallocBdev
		)
	fi

	if [[ $SPDK_TEST_PMDK -eq 1 ]]; then
		pmem_pool_file=$(mktemp /tmp/pool_file1.XXXXX)
		rm -f $pmem_pool_file
		tgt_rpc bdev_pmem_create_pool $pmem_pool_file 128 4096
		tgt_rpc bdev_pmem_create -n pmem1 $pmem_pool_file
		expected_notifications+=(bdev_register:pmem1)
	fi

	if [[ $SPDK_TEST_RBD -eq 1 ]]; then
		rbd_setup 127.0.0.1
		tgt_rpc bdev_rbd_create $RBD_POOL $RBD_NAME 4096
		expected_notifications+=(bdev_register:Ceph0)
	fi

	tgt_check_notifications "${expected_notifications[@]}"

	timing_exit "${FUNCNAME[0]}"
}

function cleanup_bdev_subsystem_config() {
	timing_enter "${FUNCNAME[0]}"

	if [[ $SPDK_TEST_BLOCKDEV -eq 1 ]]; then
		tgt_rpc bdev_lvol_delete lvs_test/clone0
		tgt_rpc bdev_lvol_delete lvs_test/lvol0
		tgt_rpc bdev_lvol_delete lvs_test/snapshot0
		tgt_rpc bdev_lvol_delete_lvstore -l lvs_test
	fi

	if [[ $(uname -s) = Linux ]]; then
		rm -f "$SPDK_TEST_STORAGE/sample_aio"
	fi

	if [[ $SPDK_TEST_PMDK -eq 1 && -n "$pmem_pool_file" && -f "$pmem_pool_file" ]]; then
		tgt_rpc bdev_pmem_delete pmem1
		tgt_rpc bdev_pmem_delete_pool $pmem_pool_file
		rm -f $pmem_pool_file
	fi

	if [[ $SPDK_TEST_RBD -eq 1 ]]; then
		rbd_cleanup
	fi

	timing_exit "${FUNCNAME[0]}"
}

function create_vhost_subsystem_config() {
	timing_enter "${FUNCNAME[0]}"

	tgt_rpc bdev_malloc_create 64 1024 --name MallocForVhost0
	tgt_rpc bdev_split_create MallocForVhost0 8

	tgt_rpc vhost_create_scsi_controller VhostScsiCtrlr0
	tgt_rpc vhost_scsi_controller_add_target VhostScsiCtrlr0 0 MallocForVhost0p3
	tgt_rpc vhost_scsi_controller_add_target VhostScsiCtrlr0 -1 MallocForVhost0p4
	tgt_rpc vhost_controller_set_coalescing VhostScsiCtrlr0 1 100

	tgt_rpc vhost_create_blk_controller VhostBlkCtrlr0 MallocForVhost0p5

	timing_exit "${FUNCNAME[0]}"
}

function create_iscsi_subsystem_config() {
	timing_enter "${FUNCNAME[0]}"
	tgt_rpc bdev_malloc_create 64 1024 --name MallocForIscsi0
	tgt_rpc iscsi_create_portal_group $PORTAL_TAG 127.0.0.1:$ISCSI_PORT
	tgt_rpc iscsi_create_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
	tgt_rpc iscsi_create_target_node Target3 Target3_alias 'MallocForIscsi0:0' $PORTAL_TAG:$INITIATOR_TAG 64 -d
	timing_exit "${FUNCNAME[0]}"
}

function create_nvmf_subsystem_config() {
	timing_enter "${FUNCNAME[0]}"

	NVMF_FIRST_TARGET_IP="127.0.0.1"
	if [[ $SPDK_TEST_NVMF_TRANSPORT == "rdma" ]]; then
		rdma_device_init
		NVMF_FIRST_TARGET_IP=$(get_available_rdma_ips | head -n 1)
	fi

	if [[ -z $NVMF_FIRST_TARGET_IP ]]; then
		echo "Error: no NIC for nvmf test"
		return 1
	fi

	tgt_rpc bdev_malloc_create 8 512 --name MallocForNvmf0
	tgt_rpc bdev_malloc_create 4 1024 --name MallocForNvmf1

	tgt_rpc nvmf_create_transport -t $SPDK_TEST_NVMF_TRANSPORT -u 8192 -c 0
	tgt_rpc nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
	tgt_rpc nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 MallocForNvmf0
	tgt_rpc nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 MallocForNvmf1
	tgt_rpc nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t $SPDK_TEST_NVMF_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s "$NVMF_PORT"

	timing_exit "${FUNCNAME[0]}"
}

function create_virtio_initiator_config() {
	timing_enter "${FUNCNAME[0]}"
	initiator_rpc bdev_virtio_attach_controller -t user -a /var/tmp/VhostScsiCtrlr0 -d scsi VirtioScsiCtrlr0
	initiator_rpc bdev_virtio_attach_controller -t user -a /var/tmp/VhostBlkCtrlr0 -d blk VirtioBlk0
	timing_exit "${FUNCNAME[0]}"
}

function json_config_test_init() {
	timing_enter "${FUNCNAME[0]}"
	timing_enter json_config_setup_target

	json_config_test_start_app target --wait-for-rpc

	#TODO: global subsystem params

	# Load nvme configuration. The load_config will issue framework_start_init automatically
	(
		$rootdir/scripts/gen_nvme.sh --json-with-subsystems
	) | tgt_rpc load_config

	tgt_check_notification_types

	if [[ $SPDK_TEST_BLOCKDEV -eq 1 ]]; then
		create_bdev_subsystem_config
	fi

	if [[ $SPDK_TEST_VHOST -eq 1 ]]; then
		create_vhost_subsystem_config
	fi

	if [[ $SPDK_TEST_ISCSI -eq 1 ]]; then
		create_iscsi_subsystem_config
	fi

	if [[ $SPDK_TEST_NVMF -eq 1 ]]; then
		create_nvmf_subsystem_config
	fi
	timing_exit json_config_setup_target

	if [[ $SPDK_TEST_VHOST_INIT -eq 1 ]]; then
		json_config_test_start_app initiator
		create_virtio_initiator_config
	fi

	tgt_rpc bdev_malloc_create 8 512 --name MallocBdevForConfigChangeCheck

	timing_exit "${FUNCNAME[0]}"
}

function json_config_test_fini() {
	timing_enter "${FUNCNAME[0]}"
	local ret=0

	if [[ -n "${app_pid[initiator]}" ]]; then
		killprocess ${app_pid[initiator]}
	fi

	if [[ -n "${app_pid[target]}" ]]; then

		# Remove any artifacts we created (files, lvol etc)
		cleanup_bdev_subsystem_config

		# SPDK_TEST_NVMF: Should we clear something?
		killprocess ${app_pid[target]}
	fi

	rm -f "${configs_path[@]}"
	timing_exit "${FUNCNAME[0]}"
	return $ret
}

function json_config_clear() {
	[[ -n "${#app_socket[$1]}" ]] # Check app type
	$rootdir/test/json_config/clear_config.py -s ${app_socket[$1]} clear_config

	# Check if config is clean.
	# Global params can't be cleared so need to filter them out.
	local config_filter="$rootdir/test/json_config/config_filter.py"

	# RPC's used to cleanup configuration (e.g. to delete split and nvme bdevs)
	# complete immediately and they don't wait for the unregister callback.
	# It causes that configuration may not be fully cleaned at this moment and
	# we should to wait a while. (See github issue #789)
	count=100
	while [ $count -gt 0 ]; do
		$rootdir/scripts/rpc.py -s "${app_socket[$1]}" save_config | $config_filter -method delete_global_parameters | $config_filter -method check_empty && break
		count=$((count - 1))
		sleep 0.1
	done

	if [ $count -eq 0 ]; then
		return 1
	fi
}

on_error_exit() {
	set -x
	set +e
	print_backtrace
	trap - ERR
	echo "Error on $1 - $2"
	json_config_test_fini
	exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
echo "INFO: JSON configuration test init"
json_config_test_init

tgt_rpc save_config > ${configs_path[target]}

echo "INFO: shutting down applications..."
if [[ $SPDK_TEST_VHOST_INIT -eq 1 ]]; then
	initiator_rpc save_config > ${configs_path[initiator]}
	json_config_clear initiator
	json_config_test_shutdown_app initiator
fi

json_config_clear target
json_config_test_shutdown_app target

echo "INFO: relaunching applications..."
json_config_test_start_app target --json ${configs_path[target]}
if [[ $SPDK_TEST_VHOST_INIT -eq 1 ]]; then
	json_config_test_start_app initiator --json ${configs_path[initiator]}
fi

echo "INFO: Checking if target configuration is the same..."
$rootdir/test/json_config/json_diff.sh <(tgt_rpc save_config) "${configs_path[target]}"
if [[ $SPDK_TEST_VHOST_INIT -eq 1 ]]; then
	echo "INFO: Checking if virtio initiator configuration is the same..."
	$rootdir/test/json_config/json_diff.sh <(initiator_rpc save_config) "${configs_path[initiator]}"
fi

echo "INFO: changing configuration and checking if this can be detected..."
# Self test to check if configuration diff can be detected.
tgt_rpc bdev_malloc_delete MallocBdevForConfigChangeCheck
if $rootdir/test/json_config/json_diff.sh <(tgt_rpc save_config) "${configs_path[target]}" > /dev/null; then
	echo "ERROR: intentional configuration difference not detected!"
	false
else
	echo "INFO: configuration change detected."
fi

json_config_test_fini

echo "INFO: Success"
