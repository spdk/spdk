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

spdk_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/spdk.sock"
initiator_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/virtio.sock"
base_json_config=$JSON_DIR/base_config.json
last_json_config=$JSON_DIR/last_config.json
base_bdevs=$JSON_DIR/bdevs_base.txt
last_bdevs=$JSON_DIR/bdevs_last.txt

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
	$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x1 -p 0 -c $JSON_DIR/vhost.conf.in -s 1024 -r /var/tmp/virtio.sock &
	virtio_pid=$!
	waitforlisten $virtio_pid
	rm $JSON_DIR/vhost.conf.in
}

function test_json_config() {
	$rpc_py get_bdevs | jq '.|sort_by(.name)' > $base_bdevs
	$rpc_py save_config -f $base_json_config
        cat $base_json_config
	$rpc_py clear_config
	$rpc_py load_config --filename $base_json_config
	$rpc_py get_bdevs | jq '.|sort_by(.name)' > $last_bdevs
	$rpc_py save_config -f $last_json_config
	diff $base_json_config $last_json_config
	diff $base_bdevs $last_bdevs
	rm $last_bdevs $base_bdevs || true
	rm $last_json_config $base_json_config || true
}

function upload_spdk_tgt() {
        $rpc_py destroy_lvol_store -l lvs_test || true
        $rpc_py construct_split_vbdev Nvme0n1 3
        $rpc_py construct_split_vbdev Nvme1n1 2
	$rpc_py construct_null_bdev Null0 32 512
	$rpc_py construct_malloc_bdev 128 512 --name Malloc0
	$rpc_py construct_malloc_bdev 64 4096 --name Malloc1
	$rpc_py construct_malloc_bdev 8 1024 --name Malloc2
	$rpc_py construct_error_bdev Malloc2
	$rpc_py construct_aio_bdev /root/sample_aio aio_disk 1024
	$rpc_py construct_lvol_store -c 1048576 Nvme0n1p2 lvs_test
	$rpc_py construct_lvol_bdev -l lvs_test lvol0 32
	$rpc_py construct_lvol_bdev -l lvs_test -t lvol1 32
	$rpc_py snapshot_lvol_bdev lvs_test/lvol0 snapshot0
	$rpc_py clone_lvol_bdev lvs_test/snapshot0 clone0
	$rpc_py create_pmem_pool /tmp/pool_file1 32 512
	$rpc_py construct_pmem_bdev -n pmem1 /tmp/pool_file1
	rbd_setup 127.0.0.1
	$rpc_py construct_rbd_bdev $RBD_POOL $RBD_NAME 4096
}

function clean_upload_spdk_tgt() {
	$rpc_py destroy_lvol_store -l lvs_test
	$rpc_py delete_pmem_pool /tmp/pool_file1
	rbd_cleanup
}

function pre_initiator_config() {
	$rpc_py construct_vhost_scsi_controller naa.Nvme0n1p0.0
	$rpc_py construct_vhost_scsi_controller naa.Nvme1n1p0.1
	$rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0
	$rpc_py add_vhost_scsi_lun naa.Nvme1n1p0.1 0 Nvme1n1p0
	$rpc_py construct_vhost_blk_controller naa.Nvme0n1p1.0 Nvme0n1p1
	$rpc_py construct_vhost_blk_controller naa.Nvme1n1p1.1 Nvme1n1p1
}

function upload_initiator() {
	$rpc_py construct_virtio_user_scsi_bdev $(get_vhost_dir 0)/naa.Nvme0n1p0.0 Nvme0n1p0
	$rpc_py construct_virtio_user_blk_bdev $(get_vhost_dir 0)/naa.Nvme0n1p1.0 Nvme0n1p1
}

function test_spdk_tgt() {
	run_spdk_tgt
	rootdir=$(readlink -f $JSON_DIR/../..)

	rpc_py="$spdk_rpc_py"
	upload_spdk_tgt
	test_json_config
        pre_initiator_config
	run_initiator
	rpc_py="$initiator_rpc_py"
	upload_initiator
	test_json_config
	clean_upload_spdk_tgt
	killprocess $virtio_pid
	killprocess $spdk_tgt_pid
}

function on_error_exit() {
	set +e
	echo "Error on $1 - $2"
        rm $last_bdevs $base_bdevs || true
        rm $last_json_config $base_json_config || true
	clean_upload_spdk_tgt
	killprocess $virtio_pid
	killprocess $spdk_tgt_pid
	rm /tmp/pool_file* || true
	print_backtrace
	exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
modprobe nbd

test_spdk_tgt

rmmod nbd
