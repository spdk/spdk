#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2022 Intel Corporation.
#  All rights reserved.

function clean_vfio_user() {
	trap - ERR
	print_backtrace
	set +e
	error "Error on $1 $2"
	vm_kill_all
	vhost_kill 0
	exit 1
}

function vfio_user_run() {
	local vhost_name=$1
	local vfio_user_dir nvmf_pid_file rpc_py

	vfio_user_dir=$(get_vhost_dir $vhost_name)
	nvmf_pid_file="$vfio_user_dir/vhost.pid"
	rpc_py="$rootdir/scripts/rpc.py -s $vfio_user_dir/rpc.sock"

	mkdir -p $vfio_user_dir

	timing_enter vfio_user_start
	$rootdir/build/bin/nvmf_tgt -r $vfio_user_dir/rpc.sock -m 0xf &
	nvmfpid=$!
	echo $nvmfpid > $nvmf_pid_file

	echo "Process pid: $nvmfpid"
	echo "waiting for app to run..."
	waitforlisten $nvmfpid $vfio_user_dir/rpc.sock

	$rpc_py nvmf_create_transport -t VFIOUSER
	timing_exit vfio_user_start
}
