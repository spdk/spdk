#!/usr/bin/env bash

rootdir=$(readlink -f $(dirname $0)/../..)
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

# Check that adding arbitrary top-level key to JSON SPDK config alongside
# "subsystems" doesn't break SPDK parsing that occurs when loading config
# to initialize subsystems. This enables applications to use the same config
# file to communicate private and SPDK data.

declare -A app_pid=([target]="")
declare -A app_socket=([target]='/var/tmp/spdk_tgt.sock')
declare -A app_params=([target]='-m 0x1 -s 1024')
declare -A configs_path=([target]="$rootdir/test/json_config/extra_key.json")

# $1 - target
# $2..$n app parameters
function json_config_test_start_app() {
	local app=$1
	shift

	[[ -n "${#app_socket[$app]}" ]] # Check app type
	[[ -z "${app_pid[$app]}" ]]     # Assert if app is not running

	$SPDK_BIN_DIR/spdk_tgt ${app_params[$app]} -r ${app_socket[$app]} "$@" &
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

on_error_exit() {
	set -x
	set +e
	print_backtrace
	trap - ERR
	echo "Error on $1 - $2"
	exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR

echo "INFO: launching applications..."
json_config_test_start_app target --json ${configs_path[target]}

echo "INFO: shutting down applications..."
json_config_test_shutdown_app target

echo "Success"
