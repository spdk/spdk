#!/bin/bash

CURRENT_DIR=$(readlink -f $(dirname ${BASH_SOURCE[0]}))
SPDK_BUILD_DIR=$CURRENT_DIR/../../
source $CURRENT_DIR/../common/autotest_common.sh

function create_package() {
	sudo yum -y install dpdk dpdk-devel
	rpmdev-setuptree
	cd $SPDK_BUILD_DIR
	sudo NRHUGE=1024 $SPDK_BUILD_DIR/scripts/setup.sh
	git archive HEAD^{tree} --format=tar.gz --prefix=spdk-18.10/ -o $HOME/rpmbuild/SOURCES/v18.10.tar.gz
	rpmbuild -ba -v $SPDK_BUILD_DIR/pkg/fedora28.spec
}

function install_packages() {
	sudo yum -y install $HOME/rpmbuild/RPMS/x86_64/*
	sudo yum -y install $HOME/rpmbuild/RPMS/noarch/*
}

function test_package() {
	sudo spdk_tgt &
	SPDK_PID=$!
	sleep 5
	sudo spdk-rpc construct_malloc_bdev -b Malloc0 32 512
	sudo kill -9 $SPDK_PID
	sudo pkill reactor_0
}

function uninstall_packages() {
	sudo yum -y remove 'spdk*' dpdk-devel dpdk
}

function on_error_exit() {
	set +e
	echo "Error on $1 - $2"
	uninstall_packages
	print_backtrace
	exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
create_package
install_packages
test_package
set +e
uninstall_packages
