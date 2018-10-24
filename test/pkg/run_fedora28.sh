#!/usr/bin/env bash

CURRENT_DIR=$(readlink -f $(dirname $0))
ROOT_DIR=$CURRENT_DIR/../../
. $CURRENT_DIR/../vhost/common/common.sh

os_image="/home/sys_sgsw/fedora-28.qcow2_pkg"

function on_error_exit() {
	set +e
	echo "Error on $1 - $2"
	vm_shutdown_all
	rm -f $CURRENT_DIR/spdk.tar.gz
	print_backtrace
	exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR

vm_no="3"
vm_setup --disk-type=spdk_vhost_scsi --force=$vm_no --os=$os_image --queue_num=8 --memory=6144
vm_run $vm_no

vm_wait_for_boot 600 $vm_no
touch $CURRENT_DIR/spdk.tar.gz
tar --exclude="spdk.tar.gz" --exclude="*.o" --exclude="*.d" -C $ROOT_DIR -zcf $CURRENT_DIR/spdk.tar.gz .
vm_scp $vm_no $CURRENT_DIR/spdk.tar.gz "127.0.0.1:/home/sys_sgci"
vm_ssh $vm_no "sudo -u sys_sgci mkdir -p /home/sys_sgci/spdk"
vm_ssh $vm_no "chown sys_sgci /home/sys_sgci/spdk.tar.gz"
vm_ssh $vm_no "sudo -u sys_sgci tar -zxf /home/sys_sgci/spdk.tar.gz -C /home/sys_sgci/spdk --strip-components=1"
vm_ssh $vm_no "cd /home/sys_sgci/spdk; sudo -u sys_sgci ./test/pkg/fedora28_test.sh"
vm_shutdown_all
rm -f $CURRENT_DIR/spdk.tar.gz

