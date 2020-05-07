#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh
source $testdir/migration-tc1.sh
source $testdir/migration-tc2.sh

vms=()
declare -A vms_os
declare -A vms_raw_disks
declare -A vms_ctrlrs
declare -A vms_ctrlrs_disks

# By default use Guest fio
fio_bin=""
MGMT_TARGET_IP=""
MGMT_INITIATOR_IP=""
RDMA_TARGET_IP=""
RDMA_INITIATOR_IP=""
function usage() {
	[[ -n $2 ]] && (
		echo "$2"
		echo ""
	)
	echo "Shortcut script for doing automated test of live migration."
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "    --os ARGS             VM configuration. This parameter might be used more than once:"
	echo "    --fio-bin=FIO         Use specific fio binary (will be uploaded to VM)"
	echo "    --mgmt-tgt-ip=IP      IP address of target."
	echo "    --mgmt-init-ip=IP     IP address of initiator."
	echo "    --rdma-tgt-ip=IP      IP address of targets rdma capable NIC."
	echo "    --rdma-init-ip=IP     IP address of initiators rdma capable NIC."
	echo "-x                        set -x for script debug"
}

for param in "$@"; do
	case "$param" in
		--help | -h)
			usage $0
			exit 0
			;;
		--os=*) os_image="${param#*=}" ;;
		--fio-bin=*) fio_bin="${param}" ;;
		--mgmt-tgt-ip=*) MGMT_TARGET_IP="${param#*=}" ;;
		--mgmt-init-ip=*) MGMT_INITIATOR_IP="${param#*=}" ;;
		--rdma-tgt-ip=*) RDMA_TARGET_IP="${param#*=}" ;;
		--rdma-init-ip=*) RDMA_INITIATOR_IP="${param#*=}" ;;
		-x) set -x ;;
		-v) SPDK_VHOST_VERBOSE=true ;;
		*)
			usage $0 "Invalid argument '$param'"
			exit 1
			;;
	esac
done

vhosttestinit

trap 'error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT

function vm_monitor_send() {
	local vm_num=$1
	local cmd_result_file="$2"
	local vm_dir="$VM_DIR/$1"
	local vm_monitor_port
	vm_monitor_port=$(cat $vm_dir/monitor_port)

	[[ -n "$vm_monitor_port" ]] || fail "No monitor port!"

	shift 2
	nc 127.0.0.1 $vm_monitor_port "$@" > $cmd_result_file
}

# Migrate VM $1
function vm_migrate() {
	local from_vm_dir="$VM_DIR/$1"
	local target_vm_dir
	local target_vm
	local target_vm_migration_port
	target_vm_dir="$(readlink -e $from_vm_dir/vm_migrate_to)"
	target_vm="$(basename $target_vm_dir)"
	target_vm_migration_port="$(cat $target_vm_dir/migration_port)"
	if [[ -n "$2" ]]; then
		local target_ip=$2
	else
		local target_ip="127.0.0.1"
	fi

	# Sanity check if target VM (QEMU) is configured to accept source VM (QEMU) migration
	if [[ "$(readlink -e ${target_vm_dir}/vm_incoming)" != "$(readlink -e ${from_vm_dir})" ]]; then
		fail "source VM $1 or destination VM is not properly configured for live migration"
	fi

	timing_enter vm_migrate
	notice "Migrating VM $1 to VM "$(basename $target_vm_dir)
	echo -e \
		"migrate_set_speed 1g\n" \
		"migrate tcp:$target_ip:$target_vm_migration_port\n" \
		"info migrate\n" \
		"quit" | vm_monitor_send $1 "$from_vm_dir/migration_result"

	# Post migration checks:
	if ! grep "Migration status: completed" $from_vm_dir/migration_result -q; then
		cat $from_vm_dir/migration_result
		fail "Migration failed:\n"
	fi

	# Don't perform the following check if target VM is on remote server
	# as we won't have access to it.
	# If you need this check then perform it on your own.
	if [[ "$target_ip" == "127.0.0.1" ]]; then
		if ! vm_os_booted $target_vm; then
			fail "VM$target_vm is not running"
			cat $target_vm $target_vm_dir/cont_result
		fi
	fi

	notice "Migration complete"
	timing_exit vm_migrate
}

function is_fio_running() {
	xtrace_disable

	if vm_exec $1 'kill -0 $(cat /root/fio.pid)'; then
		local ret=0
	else
		local ret=1
	fi

	xtrace_restore
	return $ret
}

run_test "vhost_migration_tc1" migration_tc1
run_test "vhost_migration_tc2" migration_tc2

trap - SIGINT ERR EXIT

vhosttestfini
