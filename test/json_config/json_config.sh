#!/usr/bin/env bash
set -ex
JSON_DIR=$(readlink -f $(dirname $0))
SPDK_BUILD_DIR=$JSON_DIR/../../
. $JSON_DIR/../common/autotest_common.sh

spdk_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/spdk.sock"
spdk_clear_config_py="$JSON_DIR/clear_config.py -s /var/tmp/spdk.sock"
initiator_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s /var/tmp/virtio.sock"
initiator_clear_config_py="$JSON_DIR/clear_config.py -s /var/tmp/virtio.sock"
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
	waitforlisten $spdk_tgt_pid $spdk_tgt_dir
	echo "spdk_tgt started - pid=$spdk_tgt_pid"

	rm $JSON_DIR/spdk_tgt.conf
	echo ""
}

function run_initiator() {
	cp $JSON_DIR/virtio.conf.base $JSON_DIR/vhost.conf.in
	$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x2 -p 0 -c $JSON_DIR/vhost.conf.in -s 1024 -r /var/tmp/virtio.sock &
	virtio_pid=$!
	waitforlisten $virtio_pid $spdk_tgt_dir/virtio.sock
	rm $JSON_DIR/vhost.conf.in
}

function test_json_config() {
	$rpc_py get_bdevs | jq '.|sort_by(.name)' > $base_bdevs
	$rpc_py save_config -f $base_json_config
	$clear_config_py clear_config
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
	$rpc_py construct_split_vbdev Nvme0n1 6
	$rpc_py construct_null_bdev Null0 32 512
	$rpc_py construct_malloc_bdev 128 512 --name Malloc0
	$rpc_py construct_malloc_bdev 64 4096 --name Malloc1
	$rpc_py construct_malloc_bdev 8 1024 --name Malloc2
	$rpc_py construct_error_bdev Malloc2
	dd if=/dev/zero of=/tmp/sample_aio bs=2048 count=5000
	$rpc_py construct_aio_bdev /tmp/sample_aio aio_disk 1024
	$rpc_py construct_lvol_store -c 1048576 Nvme0n1p5 lvs_test
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
	$clear_config_py clear_config
	if [ -f /tmp/pool_file1 ]; then
		rm /tmp/pool_file1
	fi
	if [ -f /tmp/sample_aoi ]; then
		rm /tmp/sample_aio
	fi
	# Uncomment after bug on ceph will be fixed
	# rbd_cleanup
}

function pre_initiator_config() {
	$rpc_py construct_vhost_scsi_controller naa.Nvme0n1p0.0
	$rpc_py construct_vhost_scsi_controller naa.Nvme0n1p1.1
	$rpc_py add_vhost_scsi_lun naa.Nvme0n1p0.0 0 Nvme0n1p0
	$rpc_py add_vhost_scsi_lun naa.Nvme0n1p1.1 0 Nvme0n1p1
	$rpc_py construct_vhost_blk_controller naa.Nvme0n1p2.0 Nvme0n1p2
	$rpc_py construct_vhost_blk_controller naa.Nvme0n1p3.1 Nvme0n1p3
	pci_scsi=$(lspci | grep 'Inc Virtio SCSI' | awk '{print $1;}')
	pci_blk=$(lspci | grep 'Inc Virtio block device' | awk '{print $1;}')
	if [ ! -z $pci_scsi ]; then
		$rpc_py construct_virtio_pci_scsi_bdev 0000:$pci_scsi Virtio0
	fi
	if [ ! -z $pci_blk ]; then
	        $rpc_py construct_virtio_pci_blk_bdev 0000:$pci_blk Virtio1
	fi
}

function upload_initiator() {
	$rpc_py construct_virtio_user_scsi_bdev $JSON_DIR/naa.Nvme0n1p0.0 Nvme0n1p0
	$rpc_py construct_virtio_user_blk_bdev $JSON_DIR/naa.Nvme0n1p2.0 Nvme0n1p2
}

function clean_upload_initiator() {
	$clear_config_py clear_config
}

function test_subsystems() {
	# Export flag to skip the known bug that exists in librados
	export ASAN_OPTIONS=new_delete_type_mismatch=0
	run_spdk_tgt
	rootdir=$(readlink -f $JSON_DIR/../..)

	rpc_py="$spdk_rpc_py"
	clear_config_py="$spdk_clear_config_py"
	create_bdev_subsystem_config
	test_json_config

	pre_initiator_config
	run_initiator
	rpc_py="$initiator_rpc_py"
	clear_config_py="$initiator_clear_config_py"
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

	clean_after_test_json_config
	rpc_py="$spdk_rpc_py"
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
