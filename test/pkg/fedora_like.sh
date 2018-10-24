#!/usr/bin/env bash

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
	if [[ $LOCAL_TEST -eq 1 ]]; then
		return
	fi

	vm_shutdown_all
}

function vm_ssh_root() {
	if [[ $LOCAL_TEST -eq 1 ]]; then
		bash -c "$*"
	else
		vm_ssh $vm_no "$*"
	fi
}

function vm_ssh_user() {
	if [[ $LOCAL_TEST -eq 1 ]]; then
		sudo -u "$RPM_BUILD_USER" bash -c $*"
	else
		vm_ssh $vm_no "sudo -u ${RPM_BUILD_USER} $*"
	fi
}

function on_error_exit() {
	set +e
	echo "Error on $1 - $2"
	print_backtrace
	vm_ssh_root pkill --pidfile /var/tmp/spdk_tgt.pid -9
	test_cleanup
	exit 1
}

trap 'on_error_exit "${FUNCNAME}" "${LINENO}"' ERR

#Fetch version defined in SPEC file. We use it later to craft file names
pkg_version=$(egrep '^Version: [[:digit:]]{2}\.[[:digit:]]{2}$' ${ROOT_DIR}/pkg/spdk.spec | cut -d' ' -f2)
[[ ! -z "$pkg_version" ]]
pkg_epoch=$(egrep '^Epoch: [[:digit:]]+$' ${ROOT_DIR}/pkg/spdk.spec | cut -d' ' -f2)
[[ ! -z "$pkg_epoch" ]]

test_setup

set -x
user_home=$(vm_ssh_user "cd && pwd")
[[ ! -z "$user_home" ]]

vm_ssh_root "yum -y install dpdk dpdk-devel"
vm_ssh_user rpmdev-setuptree

src_tarball_dir_prefix="spdk-${pkg_version}"
src_tarball="v${pkg_version}.tar.gz"
src_tarball_path="${user_home}/rpmbuild/SOURCES/${src_tarball}"

cd ${ROOT_DIR}
git archive HEAD^{tree} --format=tar.gz --prefix=${src_tarball_dir_prefix}/ | vm_ssh_user "cat > ${src_tarball_path}"

# Extract only folders we need: scripts for setup.sh and pkg for spec file and include folder
vm_ssh_user tar -C $user_home -xf $src_tarball_path $src_tarball_dir_prefix/{scripts,pkg,include}

#Build RPM and install RPMs
vm_ssh_user rpmbuild -ba -v ${user_home}/${src_tarball_dir_prefix}/pkg/spdk.spec

# Check if all packages we expect are there. Missing package will expand to pattern and fail to install.
cd ${user_home}/rpmbuild/RPMS/
pkgs=( x86_64/spdk{,-devel,-debugsource,-debuginfo}-${pkg_version}-${pkg_epoch}.*.rpm )
pkgs+=( noarch/spdk-tools-${pkg_version}-${pkg_epoch}.*.rpm )
#pkgs+=( noarch/spdk-doc )

vm_ssh_root yum -y install ${pkgs[@]}

vm_ssh_root "NRHUGE=512 ${user_home}/${src_tarball_dir_prefix}/scripts/setup.sh"
vm_ssh_root which spdk_tgt
# Run whole vm_ssh command in backtround not spdk_tgt only!
vm_ssh_root spdk_tgt -f /var/tmp/spdk_tgt.pid &
vm_ssh_root 'for i in $(seq 20); do echo -n .; sleep 0.5; [[ -S /var/tmp/spdk.sock ]] && exit 0; done; exit 1;'
vm_ssh_root spdk-rpc construct_malloc_bdev -b Malloc0 32 512
vm_ssh_root spdk-rpc kill_instance SIGINT
vm_ssh_root 'for i in $(seq 20); do echo -n .; sleep 0.5; [[ ! -S /var/tmp/spdk.sock ]] && exit 0; done; exit 1;'
vm_ssh_root yum -y remove 'spdk*' dpdk*

test_cleanup
