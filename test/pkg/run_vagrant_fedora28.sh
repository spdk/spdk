#!/usr/bin/env bash

CURRENT_DIR=$(readlink -f $(dirname $0))
ROOT_DIR=$CURRENT_DIR/../../
. $CURRENT_DIR/../common/autotest_common.sh

function on_error_exit() {
        set +e
        echo "Error on $1 - $2"
        vagrant halt
	rm -rf $CURRENT_DIR/fedora28
        rm -f $CURRENT_DIR/spdk.tar.gz
        print_backtrace
        exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR

if [ ! -f /var/lib/libvirt/images/nvme_disk.img ]; then
	sudo $ROOT_DIR/scripts/vagrant/create_nvme_img.sh 32
fi
$ROOT_DIR/scripts/vagrant/create_vbox.sh fedora28
cd $CURRENT_DIR/fedora28
vagrant up
touch $CURRENT_DIR/spdk.tar.gz
tar --exclude="spdk.tar.gz" --exclude="*.o" --exclude="*.d" --exclude="fedora28" -C $ROOT_DIR -zcf $CURRENT_DIR/spdk.tar.gz .
vagrant scp $CURRENT_DIR/spdk.tar.gz default:/home/vagrant/
vagrant ssh -c "mkdir -p /home/vagrant/spdk; tar -zxf /home/vagrant/spdk.tar.gz -C /home/vagrant/spdk --strip-components=1"
vagrant ssh -c 'cd /home/vagrant/spdk; ./test/pkg/fedora28_test.sh'
vagrant halt
rm -rf $CURRENT_DIR/fedora28
rm -f $CURRENT_DIR/spdk.tar.gz
