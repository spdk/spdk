#!/usr/bin/env bash

set -xe

CURRENT_DIR=$(readlink -f $(dirname $0))
ROOT_DIR=$(readlink -e $CURRENT_DIR/../../)

# Local test - you need to a root
: ${LOCAL_TEST=0}
: ${OS_IMAGE="/home/sys_sgci/fedora-28.qcow2_pkg"}
: ${RPM_BUILD_USER=sys_sgci}

source /etc/os-release
if [[ $LOCAL_TEST -eq 0 ]]; then
	source "$ROOT_DIR/test/vhost/common/common.sh"
else
	can_run=false
	if [[ $ID == fedora ]]; then
		if (( VERSION_ID >= 28 )); then
			can_run=true
		fi
	elif [[ $ID == rhel ]]; then
		if (( ${VERSION_ID/./} >= 75 )); then
			can_run=true
		fi
	fi

	if ! $can_run; then
		echo "ERROR: Sorry: this test is only for Fedora 28+ and RHEL 7.5+" >&2
		exit 1
	fi

	source "$ROOT_DIR/test/common/autotest_common.sh"
	set -x
fi

function test_setup() {
	if [[ $LOCAL_TEST -eq 1 ]]; then
		return
	fi

	vm_no="3"
	vm_setup --disk-type=spdk_vhost_scsi --force=$vm_no --os=$OS_IMAGE --queue_num=8 --memory=6144
	vm_run $vm_no
	vm_wait_for_boot 60 $vm_no
}

function test_cleanup() {
	vm_ssh_root yum -y remove 'spdk*' dpdk*

	if [[ $LOCAL_TEST -eq 1 ]]; then
		return
	fi

	vm_shutdown_all
}

function vm_ssh_root() {
	if [[ $LOCAL_TEST -eq 1 ]]; then
		bash -c -xe "$*"
	else
		vm_ssh $vm_no bash -xe -c "$*"
	fi
}

function vm_ssh_user() {
	if [[ $LOCAL_TEST -eq 1 ]]; then
		sudo -i -u "$RPM_BUILD_USER" bash -xe -c "$*"
	else
		vm_ssh $vm_no sudo -i -u ${RPM_BUILD_USER} bash -xe-c "$*"
	fi
}

function on_error_exit() {
	echo "Error on $1 - $2"
	print_backtrace
	set +e
	test_cleanup
	exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR

# Fetch version defined in SPEC file. We use it later to craft file names
pkg_version=$(sed -rn 's/^Version: ([[:digit:]]{2}\.[[:digit:]]{2}(\.[[:digit:]]+)?)$/\1/p' ${ROOT_DIR}/pkg/spdk.spec)
[[ ! -z "$pkg_version" ]]
pkg_epoch=$(sed -rn 's/^Epoch: ([[:digit:]]+)$/\1/p' ${ROOT_DIR}/pkg/spdk.spec)
[[ ! -z "$pkg_epoch" ]]

test_setup

set -x
vm_ssh_root yum -y install dpdk dpdk-devel

user_home=$(vm_ssh_user pwd)
[[ ! -z "$user_home" ]]

# Craft variables with these long names
src_tarball_dir_prefix="spdk-${pkg_version}"
src_tarball="v${pkg_version}.tar.gz"
src_tarball_path="${user_home}/rpmbuild/SOURCES/${src_tarball}"

# Upload source to proper rpmbuild folder
cd ${ROOT_DIR}
git archive HEAD^{tree} --format=tar.gz --prefix=${src_tarball_dir_prefix}/ | vm_ssh_user "cat > ${src_tarball_path}"

# Build and install RPMs
vm_ssh_user rpmdev-setuptree

# Extract only folders we need
vm_ssh_user tar -C $user_home -xf $src_tarball_path $src_tarball_dir_prefix/{scripts,pkg,include,test/pkg}
vm_ssh_user rpmbuild -ba -v ${user_home}/${src_tarball_dir_prefix}/pkg/spdk.spec

# Check if all packages we expect are there. Missing package will expand to pattern and fail to install.
cd ${user_home}/rpmbuild/RPMS/
pkgs=( \
	x86_64/spdk{,-devel,-debugsource,-debuginfo}-${pkg_version}-${pkg_epoch}.*.rpm
	noarch/spdk-tools-${pkg_version}-${pkg_epoch}.*.rpm
	# noarch/spdk-doc
)

vm_ssh_root yum -y install ${pkgs[@]}
vm_ssh_root ${user_home}/${src_tarball_dir_prefix}/test/pkg/spdk_tgt_test.sh

test_cleanup
