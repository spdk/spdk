JSON_DIR=$(readlink -f $(dirname ${BASH_SOURCE[0]}))
SPDK_BUILD_DIR=$JSON_DIR/../../
source $JSON_DIR/../common/autotest_common.sh

spdk_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/spdk.sock"
spdk_clear_config_py="$JSON_DIR/clear_config.py -s /var/tmp/spdk.sock"
initiator_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/virtio.sock"
initiator_clear_config_py="$JSON_DIR/clear_config.py -s /var/tmp/virtio.sock"
base_json_config=$JSON_DIR/base_config.json
last_json_config=$JSON_DIR/last_config.json
full_config=$JSON_DIR/full_config.json
base_bdevs=$JSON_DIR/bdevs_base.txt
last_bdevs=$JSON_DIR/bdevs_last.txt
tmp_config=$JSON_DIR/tmp_config.json
null_json_config=$JSON_DIR/null_json_config.json

function run_spdk_tgt() {
	echo "Running spdk target"
	$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x1 -p 0 -s 1024 -w &
	spdk_tgt_pid=$!

	echo "Waiting for app to run..."
	waitforlisten $spdk_tgt_pid
	echo "spdk_tgt started - pid=$spdk_tgt_pid but waits for subsystem initialization"

	echo ""
}

function load_nvme() {
	echo '{"subsystems": [' > nvme_config.json
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh --json >> nvme_config.json
	echo ']}' >> nvme_config.json
	$rpc_py load_config -f nvme_config.json
	rm nvme_config.json
}

function run_initiator() {
	$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x2 -p 0 -g -u -s 1024 -r /var/tmp/virtio.sock -w &
	virtio_pid=$!
	waitforlisten $virtio_pid /var/tmp/virtio.sock
}

function upload_vhost() {
	$rpc_py construct_split_vbdev Nvme0n1 8
	$rpc_py construct_vhost_scsi_controller sample1
	$rpc_py add_vhost_scsi_lun sample1 0 Nvme0n1p3
	$rpc_py add_vhost_scsi_lun sample1 1 Nvme0n1p4
	$rpc_py set_vhost_controller_coalescing sample1 1 100
	$rpc_py construct_vhost_blk_controller sample2 Nvme0n1p5
	$rpc_py construct_vhost_nvme_controller sample3 16
	$rpc_py add_vhost_nvme_ns sample3 Nvme0n1p6
}

function kill_targets() {
	if [ ! -z $virtio_pid ]; then
		killprocess $virtio_pid
	fi
	if [ ! -z $spdk_tgt_pid ]; then
		killprocess $spdk_tgt_pid
	fi
}

# This function test if json config was properly saved and loaded.
# 1. Get a list of bdevs and save it to the file "base_bdevs".
# 2. Save only configuration of the running spdk_tgt to the file "base_json_config"
#    (global parameters are not saved).
# 3. Clear configuration of the running spdk_tgt.
# 4. Save only configuration of the running spdk_tgt to the file "null_json_config"
#    (global parameters are not saved).
# 5. Check if configuration of the running spdk_tgt is cleared by checking
#    if the file "null_json_config" doesn't have any configuration.
# 6. Load the file "base_json_config" to the running spdk_tgt.
# 7. Get a list of bdevs and save it to the file "last_bdevs".
# 8. Save only configuration of the running spdk_tgt to the file "last_json_config".
# 9. Check if the file "base_json_config" matches the file "last_json_config".
# 10. Check if the file "base_bdevs" matches the file "last_bdevs".
# 11. Remove all files.
function test_json_config() {
	$rpc_py get_bdevs | jq '.|sort_by(.name)' > $base_bdevs
	$rpc_py save_config -f $full_config
	$JSON_DIR/config_filter.py -method "delete_global_parameters" -filename $full_config > $base_json_config
	$clear_config_py clear_config
	$rpc_py save_config -f $tmp_config
	$JSON_DIR/config_filter.py -method "delete_global_parameters" -filename $tmp_config > $null_json_config
	if [ "[]" != "$(jq '.subsystems | map(select(.config != null)) | map(select(.config != []))' $null_json_config)" ]; then
		echo "Config has not been cleared"
		return 1
	fi
	$rpc_py load_config -f $base_json_config
	$rpc_py get_bdevs | jq '.|sort_by(.name)' > $last_bdevs
	$rpc_py save_config -f $tmp_config
	$JSON_DIR/config_filter.py -method "delete_global_parameters" -filename $tmp_config > $last_json_config
	diff $base_json_config $last_json_config
	diff $base_bdevs $last_bdevs
	remove_config_files_after_test_json_config
}

function remove_config_files_after_test_json_config() {
	rm $last_bdevs $base_bdevs
	rm $last_json_config $base_json_config
	rm $tmp_config $full_config $null_json_config
}

function create_pmem_bdev_subsytem_config() {
        $rpc_py create_pmem_pool /tmp/pool_file1 128 512
        $rpc_py construct_pmem_bdev -n pmem1 /tmp/pool_file1
}

function clear_pmem_bdev_subsystem_config() {
	$clear_config_py clear_config
	$rpc_py  delete_pmem_pool /tmp/pool_file1
}

function create_rbd_bdev_subsystem_config() {
	rbd_setup 127.0.0.1
	$rpc_py construct_rbd_bdev $RBD_POOL $RBD_NAME 4096
}

function clear_rbd_bdev_subsystem_config() {
	$clear_config_py clear_config
	rbd_cleanup
}

function create_bdev_subsystem_config() {
	$rpc_py construct_split_vbdev Nvme0n1 2
	$rpc_py construct_null_bdev Null0 32 512
	$rpc_py construct_malloc_bdev 128 512 --name Malloc0
	$rpc_py construct_malloc_bdev 64 4096 --name Malloc1
	$rpc_py construct_malloc_bdev 8 1024 --name Malloc2
	$rpc_py construct_error_bdev Malloc2
	if [ $(uname -s) = Linux ]; then
		dd if=/dev/zero of=/tmp/sample_aio bs=2048 count=5000
		$rpc_py construct_aio_bdev /tmp/sample_aio aio_disk 1024
	fi
	$rpc_py construct_lvol_store -c 1048576 Nvme0n1p0 lvs_test
	$rpc_py construct_lvol_bdev -l lvs_test lvol0 32
	$rpc_py construct_lvol_bdev -l lvs_test -t lvol1 32
	$rpc_py snapshot_lvol_bdev lvs_test/lvol0 snapshot0
	$rpc_py clone_lvol_bdev lvs_test/snapshot0 clone0
}

function clear_bdev_subsystem_config() {
	$rpc_py destroy_lvol_bdev lvs_test/clone0
	$rpc_py destroy_lvol_bdev lvs_test/lvol0
	$rpc_py destroy_lvol_bdev lvs_test/snapshot0
	$rpc_py destroy_lvol_store -l lvs_test
	$clear_config_py clear_config
	if [ $(uname -s) = Linux ]; then
		rm -f /tmp/sample_aio
	fi
}

# In this test, target is spdk_tgt or virtio_initiator.
# 1. Save current spdk config to full_config
#    and save only global parameters to the file "base_json_config".
# 2. Exit the running spdk target.
# 3. Start the spdk target and wait for loading config.
# 4. Load global parameters and configuration to the spdk target from the file full_config.
# 5. Save json config to the file "full_config".
# 6. Save only global parameters to the file "last_json_config".
# 7. Check if the file "base_json_config" matches the file "last_json_config".
# 8. Delete all files.
function test_global_params() {
	target=$1
	$rpc_py save_config -f $full_config
	python $JSON_DIR/config_filter.py -method "delete_configs" -filename $full_config > $base_json_config
	if [ $target == "spdk_tgt" ]; then
		killprocess $spdk_tgt_pid
		run_spdk_tgt
	elif [ $target == "virtio_initiator" ]; then
		killprocess $virtio_pid
                run_initiator
	else
		echo "Target is not specified for test_global_params"
		return 1
	fi
	$rpc_py load_config -f $full_config
	$rpc_py save_config -f $full_config
	python $JSON_DIR/config_filter.py -method "delete_configs" -filename $full_config > $last_json_config
	diff $base_json_config $last_json_config
	rm $base_json_config $last_json_config
	rm $full_config
}

function on_error_exit() {
	set +e
	echo "Error on $1 - $2"
	remove_config_files_after_test_json_config
	rpc_py="$spdk_rpc_py"
	clear_config_py="$spdk_clear_config_py"
	clear_bdev_subsystem_config

	kill_targets

	print_backtrace
	exit 1
}
