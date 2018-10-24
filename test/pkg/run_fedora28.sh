#!/usr/bin/env bash

CURRENT_DIR=$(readlink -f $(dirname $0))
ROOT_DIR=$CURRENT_DIR/../../
. $CURRENT_DIR/../vhost/common/common.sh

os_image="/home/sys_sgsw/vhost_vm_image.qcow2"

function on_error_exit() {
	set +e
	echo "Error on $1 - $2"
	vm_shutdown_all
	rm -f $CURRENT_DIR/spdk.tar.gz
	print_backtrace
	exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR

vm_no="0"
vm_setup --disk-type=spdk_vhost_scsi --force=$vm_no --os=$os_image --queue_num=8 --memory=4096
vm_run $vm_no

vm_wait_for_boot 600 $vm_no
touch $CURRENT_DIR/spdk.tar.gz
tar --exclude="spdk.tar.gz" --exclude="*.o" --exclude="*.d" --exclude=".git" -C $ROOT_DIR -zcf $CURRENT_DIR/spdk.tar.gz .
vm_scp $vm_no $CURRENT_DIR/spdk.tar.gz "127.0.0.1:/root"
vm_ssh $vm_no "mkdir -p /root/spdk; tar -zxf /root/spdk.tar.gz -C /root/spdk --strip-components=1"
vm_ssh $vm_no "cd /root/spdk; ./test/pkg/fedora28_test.sh"
vm_shutdown_all
rm -f $CURRENT_DIR/spdk.tar.gz

