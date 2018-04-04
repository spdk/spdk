#!/usr/bin/env bash
set -ex
JSON_DIR=$(readlink -f $(dirname $0))
SPDK_BUILD_DIR=$JSON_DIR/../../
SPDK_VHOST_SCSI_TEST_DIR=$SPDK_BUILD_DIR/../vhost
rootdir=$SPDK_BUILD_DIR
. $JSON_DIR/../vhost/common/autotest.config
. $JSON_DIR/../nvmf/common.sh
. $JSON_DIR/../common/autotest_common.sh
. $JSON_DIR/../iscsi_tgt/common.sh

spdk_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s $SPDK_VHOST_SCSI_TEST_DIR"0"/rpc.sock"
base_json_config=$JSON_DIR/base_config.json
last_json_config=$JSON_DIR/last_config.json
base_bdevs=$JSON_DIR/bdevs_base.txt
last_bdevs=$JSON_DIR/bdevs_last.txt

function run_vhost() {
	local spdk_tgt_num=0
	local spdk_tgt_conf_path="$JSON_DIR"
	local spdk_tgt_dir="$SPDK_VHOST_SCSI_TEST_DIR""$spdk_tgt_num"
	local spdk_tgt_app="$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt"
	local spdk_tgt_log_file="$spdk_tgt_dir/vhost.log"
	local spdk_tgt_pid_file="$spdk_tgt_dir/vhost.pid"
	local spdk_tgt_conf_template="$spdk_tgt_conf_path/vhost.conf.in"
	local spdk_tgt_conf_file="$spdk_tgt_conf_path/vhost.conf"
	echo "Starting spdk target in background"
	[[ -r "$spdk_tgt_pid_file" ]] && killprocess $(cat $spdk_tgt_pid_file) || true
	[[ -d $spdk_tgt_dir ]] && rm -f $spdk_tgt_dir/*
	mkdir -p $spdk_tgt_dir

	if [[ ! -x $spdk_tgt_app ]]; then
		error "application not found: $vhost_app"
		return 1
	fi

	local reactor_mask="vhost_${spdk_tgt_num}_reactor_mask"
	reactor_mask="${!reactor_mask}"

	local master_core="vhost_${spdk_tgt_num}_master_core"
	master_core="${!master_core}"
        local cmd="$spdk_tgt_app -m $reactor_mask -p $master_core -c $spdk_tgt_conf_file -r $spdk_tgt_dir/rpc.sock"
	if [[ -z "$reactor_mask" ]] || [[ -z "$master_core" ]]; then
		error "Parameters vhost_${spdk_tgt_num}_reactor_mask or vhost_${spdk_tgt_num}_master_core not found in autotest.config file"
		return 1
	fi

	cp $spdk_tgt_conf_template $spdk_tgt_conf_file
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $spdk_tgt_conf_file

	echo "Running spdk target"
	cd $spdk_tgt_dir; $cmd &
	spdk_tgt_pid=$!
	echo $spdk_tgt_pid > $spdk_tgt_pid_file

	echo "waiting for app to run..."
	waitforlisten "$spdk_tgt_pid" "$spdk_tgt_dir/rpc.sock"
	echo "vhost started - pid=$spdk_tgt_pid"

	rm $spdk_tgt_conf_file
	echo ""
}

# Add split section into vhost config
function gen_config() {
	cp $JSON_DIR/vhost.conf.base $JSON_DIR/vhost.conf.in
	cat << END_OF_CONFIG >> $JSON_DIR/vhost.conf.in
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
	$rpc_py construct_lvol_store -c 1048576 Nvme0n1p0 lvs_test
	$rpc_py construct_lvol_bdev -l lvs_test lvol0 32
	$rpc_py construct_lvol_bdev -l lvs_test -t lvol1 32
	$rpc_py snapshot_lvol_bdev lvs_test/lvol0 snapshot0
	$rpc_py clone_lvol_bdev lvs_test/snapshot0 clone0
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
	rootdir=$(readlink -f $JSON_DIR/../..)

	rpc_py="$spdk_rpc_py"
	upload_vhost
	test_json_config
	clean_upload_vhost
	killprocess $vhost_pid
}

function on_error_exit() {
	set +e
	echo "Error on $1 - $2"
	rbd_cleanup || true
	clean_upload_vhost
	killprocess $vhost_pid
	rm $last_bdevs $base_bdevs || true
	rm $last_json_config $base_json_config || true
	rm $JSON_DIR/vhost.conf.in || true
	rm /tmp/pool_file* || true
	print_backtrace
	exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
gen_config
modprobe nbd

test_vhost

rmmod nbd
