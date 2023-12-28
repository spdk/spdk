#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2024 Intel Corporation
#  All rights reserved.
#

# $1 - target
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
	app_pid["$app"]=$!

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
			app_pid["$app"]=
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

function tgt_rpc() {
	$rootdir/scripts/rpc.py -s "${app_socket[target]}" "$@"
}

on_error_exit() {
	set -x
	set +e
	print_backtrace
	trap - ERR
	echo "Error on $1 - $2"
	exit 1
}
