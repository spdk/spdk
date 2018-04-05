#!/usr/bin/env bash
set -ex
JSON_DIR=$(readlink -f $(dirname $0))
SPDK_BUILD_DIR=$JSON_DIR/../../
. $JSON_DIR/../common/autotest_common.sh
. $JSON_DIR/../vhost/common/common.sh

spdk_tgt_dir=$SPDK_BUILD_DIR/../vhost0/
spdk_rpc_py="python $SPDK_BUILD_DIR/scripts/rpc.py -s $spdk_tgt_dir/spdk.sock"
spdk_clear_config_py="$JSON_DIR/clear_config.py -s $spdk_tgt_dir/spdk.sock"
base_json_config=$JSON_DIR/base_config.json
last_json_config=$JSON_DIR/last_config.json
base_bdevs=$JSON_DIR/bdevs_base.txt
last_bdevs=$JSON_DIR/bdevs_last.txt
tmp_config=$JSON_DIR/tmp_config.txt

function run_spdk_tgt() {
	cp $JSON_DIR/vm.conf.base $JSON_DIR/spdk_tgt.conf
	$SPDK_BUILD_DIR/scripts/gen_nvme.sh >> $JSON_DIR/spdk_tgt.conf
	[[ -d $spdk_tgt_dir ]] && rm -f $spdk_tgt_dir/*
	mkdir -p $spdk_tgt_dir

	echo "Running spdk target"
	current_dir=$(pwd)
	cd $spdk_tgt_dir
	$SPDK_BUILD_DIR/app/spdk_tgt/spdk_tgt -m 0x1 -p 0 -c $JSON_DIR/spdk_tgt.conf -s 1024 -r $spdk_tgt_dir/spdk.sock &
	spdk_tgt_pid=$!
	cd $current_dir

	echo "Waiting for app to run..."
	waitforlisten $spdk_tgt_pid $spdk_tgt_dir
	echo "spdk_tgt started - pid=$spdk_tgt_pid"

	rm $JSON_DIR/spdk_tgt.conf
	echo ""
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

function pre_initiator_config() {
	pci_scsi=$(lspci | grep 'Inc Virtio SCSI' | awk '{print $1;}')
	#pci_blk=$(lspci | grep 'Inc Virtio block device' | awk '{print $1;}')
	$rpc_py construct_virtio_pci_scsi_bdev 0000:$pci_scsi Virtio0
	#$rpc_py construct_virtio_pci_blk_bdev 0000:$pci_blk Virtio1
}

function test_subsystems() {
	run_spdk_tgt
	rootdir=$(readlink -f $JSON_DIR/../..)

	rpc_py="$spdk_rpc_py"
	clear_config_py="$spdk_clear_config_py"
	pre_initiator_config
	test_json_config
	$clear_config_py clear_config

	if [ ! -z $spdk_tgt_pid ]; then
		killprocess $spdk_tgt_pid
	fi
}

function on_error_exit() {
	set +e
	echo "Error on $1 - $2"

	clean_after_test_json_config
	rpc_py="$spdk_rpc_py"
	$clear_config_py clear_config

	if [ ! -z $spdk_tgt_pid ]; then
		killprocess $spdk_tgt_pid
	fi

	print_backtrace
	exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR

test_subsystems
