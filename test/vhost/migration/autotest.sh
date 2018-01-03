#!/usr/bin/env bash

set -e
no_shutdown=false
vms=()
declare -A vms_os
declare -A vms_raw_disks
declare -A vms_ctrlrs
declare -A vms_ctrlrs_disks
disk_split=""

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for doing automated test of live migration"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "    --work-dir=WORK_DIR   Where to find build file. Must exist. [default: $TEST_DIR]"
	echo "    --os ARGS             VM configuration. This parameter might be used more than once:"
	echo "                          num=NUM - VM number"
	echo "                          os=OS - VM os disk path"
	echo "                          bdevs=DISKS - VM test disks/devices path separated by ':'"
	echo "                          incoming - set this VM to wait for incoming migration"
	echo "                          If test-type=spdk_vhost_blk then each disk size is 20G e.g."
	echo "                          --vm num=X,os=os.qcow,bdevs=Malloc0:Nvme0n1:Malloc1"

	spdk_vhost_usage_common
	exit 0
}

function fail()
{
	error "$@"
	false
}

trap 'error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT

for param in "$@"; do
	case "$param" in
		--help|-h) usage $0 ;;
		--work-dir=*) TEST_DIR="${param#*=}" ;;
		--os=*) os_image="${param#*=}";;
		--fio-bin=*) fio_bin="${param#*=}" ;;
		--fio-job=*) fio_job="${param#*=}";;
		-n) no_shutdown=true ;;
		-x)
			set -x
			continue
			;;
		-v)
			SPDK_VHOST_VERBOSE=true
			continue
			;;
		*) usage $0 "Invalid argument '$param'" ;;
	esac
done

. $(readlink -e "$(dirname $0)/../common/common.sh") || exit 1

vm_kill_all

rpc="python $SPDK_BUILD_DIR/scripts/rpc.py "

spdk_vhost_run $BASE_DIR

notice "==============="
notice ""
notice "Setting up VMs"
notice ""

$rpc construct_malloc_bdev -b Malloc0 64 4096

$rpc construct_vhost_scsi_controller naa.Malloc0.0
$rpc add_vhost_scsi_lun naa.Malloc0.0 0 Malloc0

$rpc construct_vhost_scsi_controller naa.Malloc0.1
$rpc add_vhost_scsi_lun naa.Malloc0.1 0 Malloc0

function clean_vhost_config()
{
	notice "Removing vhost devices & controllers via RPC ..."
	# Delete bdev first to remove all LUNs and SCSI targets
	$rpc delete_bdev Malloc0

	# Delete controllers
	$rpc remove_vhost_controller naa.Malloc0.0
	$rpc remove_vhost_controller naa.Malloc0.1
}

trap 'trap - INT ERR EXIT; clean_vhost_config; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT

vm_setup --os="$os_image" --force=0 --disk-type=spdk_vhost_scsi --disks=Malloc0 --migrate-to=1
vm_setup --force=1 --disk-type=spdk_vhost_scsi --disks=Malloc0 --incoming=0

# Run everything
vm_run 0 1
vm_wait_for_boot 600 0

function vm_monitor_send()
{
	local vm_num=$1
	local cmd_result_file="$2"
	local vm_dir="$VM_BASE_DIR/$1"
	local vm_monitor_port=$(cat $vm_dir/monitor_port)

	[[ ! -z "$vm_monitor_port" ]] || fail "No monitor port!"

	shift 2
	echo -e "$@" | nc 127.0.0.1 $vm_monitor_port > $cmd_result_file
}

function vm_migrate()
{
	local from_vm_dir="$VM_BASE_DIR/$1"
	local target_vm_dir="$(readlink -e $from_vm_dir/vm_migrate_to)"
	local target_vm="$(basename $target_vm_dir)"
	local target_vm_migration_port="$(cat $target_vm_dir/migration_port)"

	# Sanity check if target VM (QEMU) is configured to accept source VM (QEMU) migration
	if [[ "$(readlink -e '${target_vm_dir}/vm_incoming')" != "$(readlink -e '${from_vm_dir}')" ]]; then
		fail "source VM $1 or destination VM is not properly configured for live migration"
	fi

	notice "Migrating VM $1 to VM "$(basename $target_vm_dir)
	vm_monitor_send $1 "$from_vm_dir/migration_result" \
		"migrate tcp:127.0.0.1:$target_vm_migration_port\n" \
		"info migrate\n" \
		"quit"

	# Post migration checks:
	if ! grep "Migration status: completed"  $from_vm_dir/migration_result -q; then
		fail "Migration failed:\n$(cat $from_vm_dir/migration_result)"
	fi

	if ! vm_os_booted $target_vm; then
		fail "VM$target_vm is not running"
	fi

	notice "Migration complete"
}

warning "OK!"

# Fire gordon!
vm_migrate 0
sleep 3

if $no_shutdown; then
	notice "==============="
	notice ""
	notice "Leaving environment working!"
	notice ""
	notice "==============="
	exit 0
fi

notice "Shutting down all VMs"

vm_kill 1
vm_shutdown_all

clean_vhost_config

notice "Testing done -> shutting down"
notice "killing vhost app"
spdk_vhost_kill

notice "EXIT DONE"
notice "==============="

trap - INT ERR EXIT
