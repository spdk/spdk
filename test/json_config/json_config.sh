#!/usr/bin/env bash

set -e

rootdir=$(readlink -f $(dirname $0)/../..)
SPDK_AUTOTEST_X=false
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

set -x

if [[ $SPDK_TEST_ISCSI -eq 1 ]]; then
	source "$rootdir/test/iscsi_tgt/common.sh"
fi

if [[ $SPDK_TEST_VHOST -ne 1 && $SPDK_TEST_VHOST_INIT -eq 1 ]]; then
	SPDK_TEST_VHOST=1
	echo "WARNING: Virtio initiator JSON_config test requires vhost target."
	echo "         Setting SPDK_TEST_VHOST=1 for duration of current script."
fi

set -x

if (( SPDK_TEST_BLOCKDEV + \
		SPDK_TEST_ISCSI +
		SPDK_TEST_NVMF +
		SPDK_TEST_VHOST +
		SPDK_TEST_VHOST_INIT +
		SPDK_TEST_PMDK +
		SPDK_TEST_RBD == 0 )); then
	echo "WARNING: No tests are enabled so not running JSON configuration tests"
	exit 0
fi

declare -A app_pid=([target]= [initiator]=)
declare -A app_socket=([target]='/var/tmp/spdk_tgt.sock' [initiator]='/var/tmp/spdk_initiator.sock')
declare -A app_params=([target]='-m 0x1 -s 1024' [initiator]='-m 0x2 -g -u -s 1024')
declare -A configs_path=([target]="$rootdir/spdk_tgt_config.json" [initiator]="$rootdir/spdk_initiator_config.json")

function tgt_rpc() {
	$rootdir/scripts/rpc.py -s "${app_socket[target]}" "$@"
}

function initiator_rpc() {
	$rootdir/scripts/rpc.py -s "${app_socket[initiator]}" "$@"
}

RE_UUID="[[:alnum:]-]+"
last_event_id=0

function tgt_check_notifications() {
        local event_line event ev_type ev_ctx
        local rc=""

        while read event_line; do
                # remove ID
                event="${event_line%:*}"

                ev_type=${event%*:}
                ev_ctx=${event#:*}

                ex_ev_type=${1%*:}
                ex_ev_ctx=${1#:*}

                last_event_id=${event_line##*:}

                # set rc=false in case of failure so all errors can be printed
                if (( $# == 0 )); then
                        echo "ERROR: got extra event: $event_line"
                        rc=false
                        continue
                elif ! echo "$ev_type" | egrep -q  "^${ex_ev_type}\$" || ! echo "$ev_ctx" | egrep -q "^${ex_ev_ctx}\$"; then
                        echo "ERROR: expected event '$1' but got '$event' (whole event line: $event_line)"
                        rc=false
                fi

                shift
        done < <(tgt_rpc get_notifications -i ${last_event_id} | jq -r '.[] | "\(.type):\(.ctx):\(.id)"')

        $rc

        if (( $# != 0 )); then
                echo "ERROR: missing events:"
                echo "$@"
                return 1
        fi
}

# $1 - target / initiator
# $2..$n app parameters
function json_config_test_start_app() {
	local app=$1
	shift

	[[ ! -z "${#app_socket[$app]}" ]] # Check app type
	[[ -z "${app_pid[$app]}" ]] # Assert if app is not running
	set -x

	local app_extra_params=""
	if [[ $SPDK_TEST_VHOST -eq 1 || $SPDK_TEST_VHOST_INIT -eq 1 ]]; then
		# If PWD is nfs/sshfs we can't create UNIX sockets there. Always use safe location instead.
		app_extra_params='-S /var/tmp'
	fi

	$rootdir/app/spdk_tgt/spdk_tgt ${app_params[$app]} ${app_extra_params} -r ${app_socket[$app]} "$@" &
	app_pid[$app]=$!

	echo "Waiting for $app to run..."
	waitforlisten ${app_pid[$app]} ${app_socket[$app]}
	echo ""
}

# $1 - target / initiator
function json_config_test_shutdown_app() {
	local app=$1

	# Check app type && assert app was started
	[[ ! -z "${#app_socket[$app]}" ]]
	[[ ! -z "${app_pid[$app]}" ]]

	# kill_instance RPC will trigger ASAN
	kill -SIGINT ${app_pid[$app]}

	for (( i=0; i<30; i++ )); do
		if ! kill -0 ${app_pid[$app]} 2>/dev/null; then
			app_pid[$app]=
			break
		fi
		sleep 0.5
	done

	if [[ ! -z "${app_pid[$app]}" ]]; then
		echo "SPDK $app shutdown timeout"
		return 1
	fi

	echo "SPDK $app shutdown done"
}

function create_bdev_subsystem_config() {
	timing_enter $FUNCNAME

	local expected_notifications=()

	if [[ $SPDK_TEST_BLOCKDEV -eq 1 ]]; then
		local lvol_store_base_bdev=Nvme0n1
		if ! tgt_rpc get_bdevs --name ${lvol_store_base_bdev} >/dev/null; then
			if [[ $(uname -s) = Linux ]]; then
				lvol_store_base_bdev=aio_disk
				echo "WARNING: No NVMe drive found. Using '$lvol_store_base_bdev' instead."
			else
				echo "ERROR: No NVMe drive found and bdev_aio is not supported on $(uname -s)."
				timing_exit $FUNCNAME
				return 1
			fi
		fi

		tgt_rpc construct_split_vbdev $lvol_store_base_bdev 2
		tgt_rpc construct_split_vbdev Malloc0 3
		tgt_rpc construct_passthru_bdev -b Malloc3 -p PTBdevFromMalloc3

		tgt_rpc construct_null_bdev Null0 32 512

		tgt_rpc construct_malloc_bdev 32 512 --name Malloc0
		tgt_rpc construct_malloc_bdev 16 4096 --name Malloc1

		expected_notifications+=(
			bdev_register:${lvol_store_base_bdev}
			bdev_register:${lvol_store_base_bdev}p0
			bdev_register:${lvol_store_base_bdev}p1
			bdev_register:Null0
			bdev_register:Malloc0p0
			bdev_register:Malloc0p1
			bdev_register:Malloc0p2
			bdev_register:Malloc0
			bdev_register:Malloc1
		)

		if [[ $(uname -s) = Linux ]]; then
			# This AIO bdev must be large enough to be used as LVOL store
			dd if=/dev/zero of=/tmp/sample_aio bs=1024 count=102400
			tgt_rpc construct_aio_bdev /tmp/sample_aio aio_disk 1024
			expected_notifications+=( bdev_register:aio_disk )
		fi

		# For LVOLs use split to check for proper order of initialization.
		# If LVOLs cofniguration will be reordered (eg moved before splits or AIO/NVMe)
		# it should fail loading JSON config from file.
		tgt_rpc construct_lvol_store -c 1048576 ${lvol_store_base_bdev}p0 lvs_test
		tgt_rpc construct_lvol_bdev -l lvs_test lvol0 32
		tgt_rpc construct_lvol_bdev -l lvs_test -t lvol1 32
		tgt_rpc snapshot_lvol_bdev     lvs_test/lvol0 snapshot0
		tgt_rpc clone_lvol_bdev        lvs_test/snapshot0 clone0

		expected_notifications+=(
			"bdev_register:$RE_UUID"
			"bdev_register:$RE_UUID"
			"bdev_register:$RE_UUID"
			"bdev_register:$RE_UUID"
		)
	fi

	if [[ $SPDK_TEST_CRYPTO -eq 1 ]]; then
		tgt_rpc construct_malloc_bdev 8 1024 --name MallocForCryptoBdev
		if [[ $(lspci -d:37c8 | wc -l) -eq 0 ]]; then
			local crypto_dirver=crypto_aesni_mb
		else
			local crypto_dirver=crypto_qat
		fi

		tgt_rpc construct_crypto_bdev -b MallocForCryptoBdev -c CryptoMallocBdev -d $crypto_dirver -k 0123456789123456
		expected_notifications+=(
			bdev_register:MallocForCryptoBdev
			bdev_register:CryptoMallocBdev
		)
	fi

	if [[ $SPDK_TEST_PMDK -eq 1 ]]; then
		pmem_pool_file=$(mktemp /tmp/pool_file1.XXXXX)
		rm -f $pmem_pool_file
		tgt_rpc create_pmem_pool $pmem_pool_file 128 4096
		tgt_rpc construct_pmem_bdev -n pmem1 $pmem_pool_file
		expected_notifications+=( bdev_register:pmem1 )
	fi

	if [[ $SPDK_TEST_RBD -eq 1 ]]; then
		rbd_setup 127.0.0.1
		tgt_rpc construct_rbd_bdev $RBD_POOL $RBD_NAME 4096
		expected_notifications+=( bdev_register:Ceph0 )
	fi

	tgt_check_notifications "${expected_notifications[@]}"

	timing_exit $FUNCNAME
}

function cleanup_bdev_subsystem_config() {
	timing_enter $FUNCNAME

	if [[ $SPDK_TEST_BLOCKDEV -eq 1 ]]; then
		tgt_rpc destroy_lvol_bdev     lvs_test/clone0
		tgt_rpc destroy_lvol_bdev     lvs_test/lvol0
		tgt_rpc destroy_lvol_bdev     lvs_test/snapshot0
		tgt_rpc destroy_lvol_store -l lvs_test
	fi

	if [[ $(uname -s) = Linux ]]; then
		rm -f /tmp/sample_aio
	fi

	if [[ $SPDK_TEST_PMDK -eq 1 && ! -z "$pmem_pool_file" && -f "$pmem_pool_file" ]]; then
		tgt_rpc delete_pmem_bdev pmem1
		tgt_rpc delete_pmem_pool $pmem_pool_file
		rm -f $pmem_pool_file
	fi

	if [[ $SPDK_TEST_RBD -eq 1 ]]; then
		rbd_cleanup
	fi

	timing_exit $FUNCNAME
}

function create_vhost_subsystem_config() {
	timing_enter $FUNCNAME

	tgt_rpc construct_malloc_bdev 64 1024 --name MallocForVhost0
	tgt_rpc construct_split_vbdev MallocForVhost0 8

	tgt_rpc construct_vhost_scsi_controller   VhostScsiCtrlr0
	tgt_rpc add_vhost_scsi_lun                VhostScsiCtrlr0 0 MallocForVhost0p3
	tgt_rpc add_vhost_scsi_lun                VhostScsiCtrlr0 -1 MallocForVhost0p4
	tgt_rpc set_vhost_controller_coalescing   VhostScsiCtrlr0 1 100

	tgt_rpc construct_vhost_blk_controller    VhostBlkCtrlr0 MallocForVhost0p5

	tgt_rpc construct_vhost_nvme_controller   VhostNvmeCtrlr0 16
	tgt_rpc add_vhost_nvme_ns                 VhostNvmeCtrlr0 MallocForVhost0p6

	timing_exit $FUNCNAME
}

function create_iscsi_subsystem_config() {
	timing_enter $FUNCNAME
	tgt_rpc construct_malloc_bdev 64 1024 --name MallocForIscsi0
	tgt_rpc add_portal_group $PORTAL_TAG 127.0.0.1:$ISCSI_PORT
	tgt_rpc add_initiator_group $INITIATOR_TAG $INITIATOR_NAME $NETMASK
	tgt_rpc construct_target_node Target3 Target3_alias 'MallocForIscsi0:0' $PORTAL_TAG:$INITIATOR_TAG 64 -d
	timing_exit $FUNCNAME
}

function create_nvmf_subsystem_config() {
	timing_enter $FUNCNAME

	RDMA_IP_LIST=$(get_available_rdma_ips)
	NVMF_FIRST_TARGET_IP=$(echo "$RDMA_IP_LIST" | head -n 1)
	if [[ -z $NVMF_FIRST_TARGET_IP ]]; then
		echo "Error: no NIC for nvmf test"
		return 1
	fi

	tgt_rpc construct_malloc_bdev 8 512 --name MallocForNvmf0
	tgt_rpc construct_malloc_bdev 4 1024 --name MallocForNvmf1

	tgt_rpc nvmf_create_transport -t RDMA -u 8192 -p 4 -c 0
	tgt_rpc nvmf_subsystem_create       nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
	tgt_rpc nvmf_subsystem_add_ns       nqn.2016-06.io.spdk:cnode1 MallocForNvmf0
	tgt_rpc nvmf_subsystem_add_ns       nqn.2016-06.io.spdk:cnode1 MallocForNvmf1
	tgt_rpc nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t RDMA -a $NVMF_FIRST_TARGET_IP -s "$NVMF_PORT"

	timing_exit $FUNCNAME
}

function create_virtio_initiator_config() {
	timing_enter $FUNCNAME
	initiator_rpc construct_virtio_dev -t user -a /var/tmp/VhostScsiCtrlr0 -d scsi VirtioScsiCtrlr0
	initiator_rpc construct_virtio_dev -t user -a /var/tmp/VhostBlkCtrlr0  -d blk  VirtioBlk0
	# TODO: initiator_rpc construct_virtio_dev -t user -a /var/tmp/VhostNvmeCtrlr0 -d nvme VirtioNvme0
	timing_exit $FUNCNAME
}


function json_config_test_init()
{
	timing_enter $FUNCNAME
	timing_enter json_config_setup_target

	json_config_test_start_app target --wait-for-rpc

	#TODO: global subsystem params

	# Load nvme configuration. The load_config will issue start_subsystem_init automatically
	(
		echo '{"subsystems": [';
		$rootdir/scripts/gen_nvme.sh --json
		echo ']}'
	) | tgt_rpc load_config

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

	tgt_rpc construct_malloc_bdev 8 512 --name MallocBdevForConfigChangeCheck

	timing_exit $FUNCNAME
}

function json_config_test_fini() {
	timing_enter $FUNCNAME
	local ret=0

	if [[ ! -z "${app_pid[initiator]}" ]]; then
		if ! json_config_test_shutdown_app initiator; then
			kill -9 ${app_pid[initiator]}
			app_pid[initiator]=
			ret=1
		fi
	fi

	if [[ ! -z "${app_pid[target]}" ]]; then

		# Remove any artifacts we created (files, lvol etc)
		cleanup_bdev_subsystem_config

		# SPDK_TEST_NVMF: Should we clear something?

		if ! json_config_test_shutdown_app target; then
			kill -9 ${app_pid[target]}
			app_pid[target]=
			ret=1
		fi
	fi

	rm -f "${configs_path[@]}"
	timing_exit $FUNCNAME
	return $ret
}

function json_config_clear() {
	[[ ! -z "${#app_socket[$1]}" ]] # Check app type
	$rootdir/test/json_config/clear_config.py -s ${app_socket[$1]} clear_config

	# Check if config is clean.
	# Global params can't be cleared so need to filter them out.
	local config_filter="$rootdir/test/json_config/config_filter.py"
	$rootdir/scripts/rpc.py -s "${app_socket[$1]}" save_config | \
		$config_filter -method delete_global_parameters | $config_filter -method check_empty
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
tgt_rpc delete_malloc_bdev MallocBdevForConfigChangeCheck
if $rootdir/test/json_config/json_diff.sh <(tgt_rpc save_config) "${configs_path[target]}" >/dev/null; then
	echo "ERROR: intentional configuration difference not detected!"
	false
else
	echo "INFO: configuration change detected."
fi

json_config_test_fini

echo "INFO: Success"
