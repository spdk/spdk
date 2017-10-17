#!/usr/bin/env bash
set +x

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"

test_type=spdk_vhost_blk

if [[ $EUID -ne 0 ]]; then
	echo "Go away user come back as root"
	exit 1
fi
. $BASE_DIR/../common/common.sh
source $BASE_DIR/common.sh

function blk_ro_tc1()
{
	print_tc_name ${FUNCNAME[0]}
	local vhost_blk_name=vhost.0
	
	$rpc_py get_bdevs
#	$rpc_py construct_vhost_blk_controller vhost.0 Nvme0n1
	$rpc_py construct_vhost_blk_controller naa.Nvme0n1.0 Nvme0n1
	$rpc_py get_vhost_controllers
	
	setup_cmd="$BASE_DIR/../common/vm_setup.sh $x --work-dir=$TEST_DIR --test-type=$test_type"
	setup_cmd+=" -f 0"
    setup_cmd+=" --os=/home/pniedzwx/data/fedora-25.qcow2"
    setup_cmd+=" --disk=Nvme0n1"
	
	echo $setup_cmd
	$setup_cmd
	
	$BASE_DIR/../common/vm_run.sh $x --work-dir=$TEST_DIR 0
	vm_wait_for_boot 600
}

function blk_ro_tc2()
{
	print_tc_name ${FUNCNAME[0]}
}

#$BASE_DIR/../common/run_vhost.sh --conf-dir=$BASE_DIR
spdk_vhost_run $BASE_DIR
blk_ro_tc1
#blk_ro_tc2
#spdk_vhost_kill
