#!/usr/bin/env bash

set -xe

CURRENT_DIR=$(readlink -f $(dirname $0))
ROOT_DIR=$(readlink -e $CURRENT_DIR/../../)

# Local test - you need to be a root
: ${LOCAL_TEST=0}
: ${OS_IMAGE="/home/sys_sgci/fedora-28.qcow2_pkg"}
: ${RPM_BUILD_USER=sys_sgci}

vm_no=3

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
	function vm_ssh() {
		# Remove the VM number argument
		shift

		bash -c "$*"
	}
fi

function test_setup() {
	if [[ $LOCAL_TEST -eq 0 ]]; then
		vm_setup --disk-type=spdk_vhost_scsi --force=$vm_no --os=$OS_IMAGE --queue_num=8 --memory=6144
		vm_run $vm_no
		vm_wait_for_boot 60 $vm_no
	fi

	# Just in case env is not clean
	vm_ssh_root "yum -y remove spdk spdk-debuginfo spdk-debugsource dpdk || true"
}

function test_cleanup() {
	vm_ssh_root "yum -y remove spdk spdk-debuginfo spdk-debugsource dpdk"

	if [[ $LOCAL_TEST -eq 0 ]]; then
		vm_shutdown_all
	fi
}

function vm_ssh_root() {
	vm_ssh $vm_no "bash -c 'set -ex; $*'"
}

function vm_ssh_user() {
	vm_ssh $vm_no sudo -i -u "$RPM_BUILD_USER" "bash -c 'set -ex; $*'"
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
pkg_version=$(sed -rn 's/^Version: ([[:alnum:]]+)$/\1/p' ${ROOT_DIR}/pkg/spdk.spec)
[[ ! -z "$pkg_version" ]]
pkg_epoch=$(sed -rn 's/^Epoch: ([0-9]+)$/\1/p' ${ROOT_DIR}/pkg/spdk.spec)
[[ ! -z "$pkg_epoch" ]]
src_tarball=$(basename $(sed -rn 's/^Source: ([^ ]+)$/\1/p' ${ROOT_DIR}/pkg/spdk.spec))

test_setup

set -x
vm_ssh_root yum -y install dpdk dpdk-devel

user_home=$(vm_ssh_user 'cd; pwd')
[[ ! -z "$user_home" ]]

# Craft variables with these long names
src_tarball_dir_prefix="spdk-${pkg_version}"
src_tarball_path="${user_home}/rpmbuild/SOURCES/${src_tarball}"

# Build and install RPMs
vm_ssh_user rpmdev-setuptree

# Upload source to proper rpmbuild folder
cd ${ROOT_DIR}
git archive HEAD^{tree} --format=tar.gz --prefix=${src_tarball_dir_prefix}/ | vm_ssh_user "cat > ${src_tarball_path}"

# Extract only folders we need
vm_ssh_user tar -C $user_home -xf $src_tarball_path $src_tarball_dir_prefix/{scripts,pkg,include,test/pkg}
vm_ssh_user rpmbuild -ba -v ${user_home}/${src_tarball_dir_prefix}/pkg/spdk.spec

# Check if all packages we expect are there. Missing package will expand to pattern and fail to install.
vm_ssh_root "cd ${user_home}/rpmbuild/RPMS/; \
	yum -y install \
		x86_64/spdk{,-devel,-debugsource,-debuginfo}-${pkg_version}-${pkg_epoch}.*.rpm \
		noarch/spdk-tools-${pkg_version}-${pkg_epoch}.*.rpm"

vm_ssh_root ${user_home}/${src_tarball_dir_prefix}/test/pkg/spdk_tgt_test.sh
test_cleanup
