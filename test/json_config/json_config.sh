#!/usr/bin/env bash
set -ex
JSON_DIR=$(readlink -f $(dirname $0))
SPDK_BUILD_DIR=$JSON_DIR/../../
. $JSON_DIR/../common/autotest_common.sh

spdk_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/spdk.sock"
base_json_config=$JSON_DIR/base_config.json
last_json_config=$JSON_DIR/last_config.json
base_bdevs=$JSON_DIR/bdevs_base.txt
last_bdevs=$JSON_DIR/bdevs_last.txt
tmp_config=$JSON_DIR/tmp_config.txt

function run_spdk_tgt() {
	cp $JSON_DIR/spdk_tgt.conf.base $JSON_DIR/spdk_tgt.conf
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $JSON_DIR/spdk_tgt.conf

	echo "Running spdk target"
	$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x1 -p 0 -c $JSON_DIR/spdk_tgt.conf -s 1024 -r /var/tmp/spdk.sock &
	spdk_tgt_pid=$!

	echo "Waiting for app to run..."
	waitforlisten $spdk_tgt_pid
	echo "spdk_tgt started - pid=$spdk_tgt_pid"

	rm $JSON_DIR/spdk_tgt.conf
	echo ""
}

function test_json_config() {
	$rpc_py get_bdevs | jq '.|sort_by(.name)' > $base_bdevs
	$rpc_py save_config -f $base_json_config
	$JSON_DIR/clear_config.py clear_config
	$rpc_py save_config -f $tmp_config
	if [ "[]" != "$(jq '.subsystems | map(select(.config != null)) | map(select(.config != []))' $tmp_config)" ]; then
		echo "Config has not been cleared"
		return 1
	fi
	$rpc_py load_config --filename $base_json_config
	$rpc_py get_bdevs | jq '.|sort_by(.name)' > $last_bdevs
	$rpc_py save_config -f $last_json_config
	diff $base_json_config $last_json_config
	diff $base_bdevs $last_bdevs
	clean_after_test_json_config
}

function clean_after_test_json_config() {
	rm $last_bdevs $base_bdevs
	rm $last_json_config $base_json_config
	rm $tmp_config
}

function create_bdev_subsystem_config() {
	$rpc_py construct_split_vbdev Nvme1n1 2
	$rpc_py construct_null_bdev Null0 32 512
	$rpc_py construct_malloc_bdev 128 512 --name Malloc0
	$rpc_py construct_malloc_bdev 64 4096 --name Malloc1
	$rpc_py construct_malloc_bdev 8 1024 --name Malloc2
	$rpc_py construct_error_bdev Malloc2
	dd if=/dev/zero of=/tmp/sample_aio bs=2048 count=5000
	$rpc_py construct_aio_bdev /tmp/sample_aio aio_disk 1024
	$rpc_py construct_lvol_store -c 1048576 Nvme0n1 lvs_test
	$rpc_py construct_lvol_bdev -l lvs_test lvol0 32
	$rpc_py construct_lvol_bdev -l lvs_test -t lvol1 32
	$rpc_py snapshot_lvol_bdev lvs_test/lvol0 snapshot0
	$rpc_py clone_lvol_bdev lvs_test/snapshot0 clone0
	$rpc_py create_pmem_pool /tmp/pool_file1 32 512
	$rpc_py construct_pmem_bdev -n pmem1 /tmp/pool_file1
	# Uncomment after bug on librados will be fixed
	# rbd_setup 127.0.0.1
	# $rpc_py construct_rbd_bdev $RBD_POOL $RBD_NAME 4096
}

function clean_bdev_subsystem_config() {
	$rpc_py delete_bdev lvs_test/clone0
	$rpc_py delete_bdev lvs_test/lvol0
	$rpc_py delete_bdev lvs_test/lvol1
	$rpc_py destroy_lvol_store -l lvs_test
	$JSON_DIR/clear_config.py clear_config
	if [ -f /tmp/pool_file1 ]; then
		rm /tmp/pool_file1
	fi
	if [ -f /tmp/sample_aoi ]; then
		rm /tmp/sample_aio
	fi
	# Uncomment after bug on ceph will be fixed
	# rbd_cleanup
}

function test_subsystems() {
	# Export flag to skip the known bug that exists in librados
	export ASAN_OPTIONS=new_delete_type_mismatch=0
	run_spdk_tgt
	rootdir=$(readlink -f $JSON_DIR/../..)

	rpc_py="$spdk_rpc_py"
	create_bdev_subsystem_config
	test_json_config

	clean_bdev_subsystem_config
	killprocess $spdk_tgt_pid
}

function on_error_exit() {
	set +e
	echo "Error on $1 - $2"
	clean_after_test_json_config
	clean_bdev_subsystem_config
	killprocess $spdk_tgt_pid
	print_backtrace
	exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
modprobe nbd

test_subsystems

rmmod nbd
