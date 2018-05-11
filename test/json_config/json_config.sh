
#!/usr/bin/env bash
set -ex
JSON_DIR=$(readlink -f $(dirname $0))
SPDK_BUILD_DIR=$JSON_DIR/../../
. $JSON_DIR/../common/autotest_common.sh

spdk_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/spdk.sock"
initiator_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/virtio.sock"
base_json_config=$JSON_DIR/base_config.json
last_json_config=$JSON_DIR/last_config.json
base_bdevs=$JSON_DIR/bdevs_base.txt
last_bdevs=$JSON_DIR/bdevs_last.txt
base_nbd=$JSON_DIR/nbd_base.txt
last_nbd=$JSON_DIR/nbd_last.txt

function run_spdk_tgt() {
	cp $JSON_DIR/vhost.conf.base $JSON_DIR/vhost.conf
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $JSON_DIR/vhost.conf

	echo "Running spdk target"
	$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x1 -p 0 -c $JSON_DIR/vhost.conf -s 1024 -r /var/tmp/spdk.sock &
	spdk_tgt_pid=$!

	echo "Waiting for app to run..."
	waitforlisten $spdk_tgt_pid
	echo "spdk_tgt started - pid=$spdk_tgt_pid"

	rm $JSON_DIR/vhost.conf
	echo ""
}

function run_initiator() {
	cp $JSON_DIR/virtio.conf.base $JSON_DIR/vhost.conf.in
	$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x2 -p 0 -c $JSON_DIR/vhost.conf.in -s 1024 -r /var/tmp/virtio.sock &
	virtio_pid=$!
	waitforlisten $virtio_pid /var/tmp/virtio.sock
	rm $JSON_DIR/vhost.conf.in
}

function test_json_config() {
	$rpc_py get_bdevs | jq '.|sort_by(.name)' > $base_bdevs
	$rpc_py save_config -f $base_json_config
	$rpc_py get_nbd_disks > $base_nbd
	$rpc_py clear_config
	$rpc_py load_config --filename $base_json_config
	$rpc_py get_bdevs | jq '.|sort_by(.name)' > $last_bdevs
	$rpc_py save_config -f $last_json_config
	$rpc_py get_nbd_disks > $last_nbd
	diff $base_json_config $last_json_config
	diff $base_bdevs $last_bdevs
	diff $base_nbd $last_nbd
	rm $last_bdevs $base_bdevs || true
	rm $last_json_config $base_json_config || true
	rm $last_nbd $base_nbd || true
}

function create_bdev_subsystem_config() {
        #$rpc_py construct_split_vbdev Nvme0n1 3
	$rpc_py construct_split_vbdev Nvme1n1 8
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
	rbd_setup 127.0.0.1
	$rpc_py construct_rbd_bdev $RBD_POOL $RBD_NAME 4096
}

function clean_bdev_subsystem_config() {
	$rpc_py delete_bdev lvs_test/clone0
	$rpc_py delete_bdev lvs_test/lvol0
	$rpc_py delete_bdev lvs_test/lvol1
	$rpc_py destroy_lvol_store -l lvs_test
	$rpc_py clear_config
	if [ -f /tmp/pool_file1 ]; then
		rm /tmp/pool_file1
	fi
	if [ -f /tmp/sample_aoi ]; then
		rm /tmp/sample_aio
	fi
	rbd_cleanup
}

function pre_initiator_config() {
	$rpc_py construct_vhost_scsi_controller naa.Nvme1n1p0.0
	$rpc_py construct_vhost_scsi_controller naa.Nvme1n1p1.1
	$rpc_py add_vhost_scsi_lun naa.Nvme1n1p0.0 0 Nvme1n1p0
	$rpc_py add_vhost_scsi_lun naa.Nvme1n1p1.1 0 Nvme1n1p1
	$rpc_py construct_vhost_blk_controller naa.Nvme1n1p2.0 Nvme1n1p2
	$rpc_py construct_vhost_blk_controller naa.Nvme1n1p3.1 Nvme1n1p3
}

function upload_initiator() {
	$rpc_py construct_virtio_user_scsi_bdev $JSON_DIR/naa.Nvme1n1p0.0 Nvme1n1p0
	$rpc_py construct_virtio_user_blk_bdev $JSON_DIR/naa.Nvme1n1p2.0 Nvme1n1p2
}

function clean_upload_initiator() {
	$rpc_py clear_config
}

function upload_vhost() {
	$rpc_py construct_vhost_scsi_controller sample1
	$rpc_py add_vhost_scsi_lun sample1 0 Nvme1n1p3
	$rpc_py add_vhost_scsi_lun sample1 1 Nvme1n1p4
	$rpc_py set_vhost_controller_coalescing sample1 1 100
	$rpc_py construct_vhost_blk_controller sample2 Nvme1n1p5
	$rpc_py construct_vhost_nvme_controller sample3 16
	$rpc_py add_vhost_nvme_ns sample3 Nvme1n1p6
}

function clean_vhost() {
	$rpc_py remove_vhost_controller sample3
	$rpc_py remove_vhost_controller sample2
	$rpc_py remove_vhost_scsi_target sample1 1
	$rpc_py remove_vhost_scsi_target sample1 0
	$rpc_py remove_vhost_controller sample1
}

function upload_nbd() {
	$rpc_py construct_malloc_bdev 128 512 --name Malloc0
	$rpc_py start_nbd_disk Malloc0 /dev/nbd0
}

function clean_nbd() {
	$rpc_py stop_nbd_disk /dev/nbd0
}

function test_subsystems() {
	run_spdk_tgt
	rootdir=$(readlink -f $JSON_DIR/../..)
	rpc_py="$spdk_rpc_py"
	create_bdev_subsystem_config
	test_json_config
        upload_vhost
        test_json_config
        clean_vhost
	upload_nbd
	test_json_config
	clean_nbd
	pre_initiator_config
	run_initiator
	rpc_py="$initiator_rpc_py"
	upload_initiator
	test_json_config
	clean_upload_initiator
	rpc_py="$spdk_rpc_py"
	clean_bdev_subsystem_config
	if [ ! -z $virtio_pid ]; then
		killprocess $virtio_pid
	fi
	if [ ! -z $spdk_tgt_pid ]; then
		killprocess $spdk_tgt_pid
	fi
}

function on_error_exit() {
	set +e
	echo "Error on $1 - $2"
	rm $last_bdevs $base_bdevs || true
	rm $last_json_config $base_json_config || true
	rm $last_nbd $base_nbd || true
	clean_bdev_subsystem_config
	if [ ! -z $virtio_pid ]; then
		killprocess $virtio_pid
	fi
	if [ ! -z $spdk_tgt_pid ]; then
		killprocess $spdk_tgt_pid
	fi
	print_backtrace
	exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
modprobe nbd

test_subsystems

rmmod nbd
