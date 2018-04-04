#!/usr/bin/env bash
set -e
cd ../vhost/common
. ./common.sh
cd ../../json_config
BASE_DIR=$(readlink -f $(dirname $0))
. $BASE_DIR/../nvmf/common.sh
. $BASE_DIR/../common/autotest_common.sh
. $BASE_DIR/../iscsi_tgt/common.sh

vhost_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"
nvme_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/spdk.sock"
base_json_config=$BASE_DIR/base_config.json
last_json_config=$BASE_DIR/last_config.json
base_bdevs=$BASE_DIR/bdevs_base.txt
last_bdevs=$BASE_DIR/bdevs_last.txt

function run_vhost() {
	local vhost_num=0
	local vhost_conf_path="$BASE_DIR"
	local vhost_dir="$(get_vhost_dir $vhost_num)"
	if [[ -z "$vhost_conf_path" ]]; then
		error "Missing mandatory parameter '--conf-path'"
		return 1
	fi
	local vhost_app="$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt"
	local vhost_log_file="$vhost_dir/vhost.log"
	local vhost_pid_file="$vhost_dir/vhost.pid"
	local vhost_socket="$vhost_dir/usvhost"
	local vhost_conf_template="$vhost_conf_path/vhost.conf.in"
	local vhost_conf_file="$vhost_conf_path/vhost.conf"
	notice "starting vhost app in background"
	[[ -r "$vhost_pid_file" ]] && spdk_vhost_kill $vhost_num
	[[ -d $vhost_dir ]] && rm -f $vhost_dir/*
	mkdir -p $vhost_dir

	if [[ ! -x $vhost_app ]]; then
		error "application not found: $vhost_app"
		return 1
	fi

	local reactor_mask="vhost_${vhost_num}_reactor_mask"
	reactor_mask="${!reactor_mask}"

	local master_core="vhost_${vhost_num}_master_core"
	master_core="${!master_core}"
        local cmd="$vhost_app -m $reactor_mask -p $master_core -c $vhost_conf_file -r $vhost_dir/rpc.sock"
	if [[ -z "$reactor_mask" ]] || [[ -z "$master_core" ]]; then
		error "Parameters vhost_${vhost_num}_reactor_mask or vhost_${vhost_num}_master_core not found in autotest.config file"
		return 1
	fi

	cp $vhost_conf_template $vhost_conf_file
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $vhost_conf_file

	notice "==============="
	notice ""
	notice "running spdk target"
	notice ""
	cd $vhost_dir; $cmd &
	vhost_pid=$!
	echo $vhost_pid > $vhost_pid_file

	notice "waiting for app to run..."
	waitforlisten "$vhost_pid" "$vhost_dir/rpc.sock"
	notice "vhost started - pid=$vhost_pid"

	rm $vhost_conf_file
	notice ""
}

# Add split section into vhost config
function gen_config() {
	cp $BASE_DIR/vhost.conf.base $BASE_DIR/vhost.conf.in
	cat << END_OF_CONFIG >> $BASE_DIR/vhost.conf.in
[Split]
  Split Nvme0n1 2
  Split Nvme1n1 2
END_OF_CONFIG
}

function test_json_config() {
	$rpc_py get_bdevs >> $base_bdevs
	$rpc_py save_config -f $base_json_config
	$rpc_py clear_config
	$rpc_py load_config --filename $base_json_config
	$rpc_py get_bdevs >> $last_bdevs
	$rpc_py save_config -f $last_json_config
	diff $base_json_config $last_json_config
	diff $base_bdevs $last_bdevs
	rm $last_bdevs $base_bdevs || true
	rm $last_json_config $base_json_config || true
	$rpc_py clear_config
}

function upload_vhost() {
	$rpc_py construct_null_bdev Null0 32 512
	$rpc_py construct_malloc_bdev 128 512 --name Malloc0
	$rpc_py construct_malloc_bdev 64 4096 --name Malloc1
	$rpc_py construct_malloc_bdev 8 1024 --name Malloc2
	$rpc_py construct_error_bdev Malloc2
	$rpc_py construct_aio_bdev /root/sample_aio aio_disk 1024
	$rpc_py construct_lvol_store -c 1048576 Malloc0 lvs_test
	$rpc_py construct_lvol_bdev -l lvs_test lvol0 32
	$rpc_py construct_lvol_bdev -l lvs_test -t lvol1 32
        $rpc_py create_pmem_pool /tmp/pool_file1 32 512
        $rpc_py construct_pmem_bdev -n pmem1 /tmp/pool_file1
	rbd_setup 127.0.0.1
	$rpc_py construct_rbd_bdev $RBD_POOL $RBD_NAME 4096
}

function clean_upload_vhost() {
	$rpc_py destroy_lvol_store -l lvs_test
        $rpc_py delete_pmem_pool /tmp/pool_file1
        rbd_cleanup
}

function test_vhost() {
	run_vhost
	rootdir=$(readlink -f $BASE_DIR/../..)

	rpc_py="$vhost_rpc_py"
	upload_vhost
	test_json_config
	clean_upload_vhost
	spdk_vhost_kill
}

function on_error_exit() {
	set +e
	echo "Error on $1 - $2"
	rbd_cleanup || true
        clean_upload_vhost
	for vhost_num in $(spdk_vhost_list_all); do
		spdk_vhost_kill $vhost_num
	done
	rm $last_bdevs $base_bdevs || true
	rm $last_json_config $base_json_config || true
	rm $BASE_DIR/vhost.conf.in || true
	rm /tmp/pool_file* || true
	print_backtrace
	exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
gen_config
modprobe nbd

test_vhost
test_nvmf
test_iscsi

rmmod nbd
