#!/bin/bash

CURRENT_DIR=$(readlink -f $(dirname ${BASH_SOURCE[0]}))
SPDK_BUILD_DIR=$CURRENT_DIR/../../
source $CURRENT_DIR/../common/autotest_common.sh

function install_dependencies() {
	sudo yum -y install dpdk dpdk-devel libaio-devel libibverbs-devel libiscsi-devel --skip-broken
	sudo yum -y install librdmacm-devel libuuid-devel numactl-devel openssl-devel --skip-broken
	sudo yum -y install rpm-build --skip-broken
	sudo yum -y install rpmdevtools rpmlint --skip-broken
	sudo yum -y install gcc-c++ --skip-broken
	sudo yum -y install spdk-tools --skip-broken
	sudo yum -y install python3-configshell python3-pexpect
}

function create_package() {
	sudo dnf -y install fedora-packager @development-tools
	rpmdev-setuptree
	cd $SPDK_BUILD_DIR
	sudo NRHUGE=1024 $SPDK_BUILD_DIR/scripts/setup.sh
	git archive HEAD^{tree} --format=tar.gz --prefix=spdk-18.10/ -o $HOME/rpmbuild/SOURCES/v18.10.tar.gz
	rpmbuild -ba -v $SPDK_BUILD_DIR/pkg/fedora28.spec
}

function install_package() {
	sudo rpm -U $HOME/rpmbuild/RPMS/x86_64/*
	sudo rpm -U $HOME/rpmbuild/RPMS/noarch/*
}

function test_package() {
	#sudo spdk_example_verify
	sudo spdk_tgt &
	SPDK_PID=$!
	sleep 5
	sudo spdk-rpc construct_malloc_bdev -b Malloc0 32 512
	sudo kill -9 $SPDK_PID
	#sudo spdk_example_hotplug -t 2
}

function uninstall_package() {
	sudo yum -y remove 'spdk*' dpdk-devel dpdk
}

function on_error_exit() {
	set +e
	echo "Error on $1 - $2"
	uninstall_package
	print_backtrace
	exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR
install_dependencies
create_package
install_package
test_package
set +e
uninstall_package
set -e
exit 0
