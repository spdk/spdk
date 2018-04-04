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
	notice "==============="
	notice ""
	notice "running SPDK"
	notice ""
	spdk_vhost_run --conf-path=$BASE_DIR
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
	rbd_setup
	$rpc_py construct_rbd_bdev $RBD_POOL $RBD_NAME 4096
}

function clean_upload_vhost() {
	$rpc_py destroy_lvol_store lvs_test
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
