\#!/bin/bash

JSON_DIR=$(readlink -f $(dirname ${BASH_SOURCE[0]}))
SPDK_BUILD_DIR=$JSON_DIR/../../
source $JSON_DIR/../common/autotest_common.sh

function install_dependencies() {
	sudo yum -y install dpdk dpdk-devel libaio-devel libibverbs-devel libiscsi-devel --skip-broken
	sudo yum -y install librdmacm-devel libuuid-devel numactl-devel openssl-devel --skip-broken
	sudo yum -y install rpmbuild --skip-broken
}

function create_package() {
        mkdir $SPDK_BUILD_DIR/../rpmbuild
        mkdir $SPDK_BUILD_DIR/../rpmbuild/SOURCES
        cd $SPDK_BUILD_DIR
	git archive HEAD^{tree} --format=tar.gz --prefix=spdk-18.07/ -o $SPDK_BUILD_DIR/../rpmbuild/SOURCES/v18.07.tar.gz
	rpmbuild --undefine=_disable_source_fetch -ba -v $SPDK_BUILD_DIR/pkg/fedora28.spec
}

function install_package() {
	for rpm_file in $SPDK_BUILD_DIR/../rpmbuild/RPMS/x86_64/*; do
		ls $rpm_file
		sudo rpm -U $rpm_file
	done
}

function test_package() {
	sudo spdk_example_verify
	sudo spdk_example_hotplug -t 2
}

function uninstall_package() {
	for rpm_file in spdk-examples-debuginfo spdk-examples spdk-devel spdk-debugsource spdk-debuginfo spdk; do
                sudo rpm -e $rpm_file
        done
}

function clear_after_test() {
	rm -rf $SPDK_BUILD_DIR/../rpmbuild
}

function on_error_exit() {
	set +e
	echo "Error on $1 - $2"
	uninstall_package
	clear_after_test
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
clear_after_test
