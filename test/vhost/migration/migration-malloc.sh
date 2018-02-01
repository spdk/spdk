#!/usr/bin/env bash

set -e

vms=()
declare -A vms_os
declare -A vms_raw_disks
declare -A vms_ctrlrs
declare -A vms_ctrlrs_disks

# By default use Guest fio
fio_bin=""

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for doing automated test of live migration."
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "    --work-dir=WORK_DIR   Where to find build file. Must exist. [default: $TEST_DIR]"
	echo "    --os ARGS             VM configuration. This parameter might be used more than once:"
	echo "    --fio-bin=FIO         Use specific fio binary (will be uploaded to VM)"
	echo "    --fio-job=            Fio config to use for test."
	echo "                          num=NUM - VM number"
	echo "                          os=OS - VM os disk path"
	echo "                          bdevs=DISKS - VM test disks/devices path separated by ':'"
	echo "                          incoming - set this VM to wait for incoming migration"
	echo "                          If test-type=spdk_vhost_blk then each disk size is 20G e.g."
	echo "                          --vm num=X,os=os.qcow,bdevs=Malloc0:Nvme0n1:Malloc1"
	echo "-x                        set -x for script debug"
}

for param in "$@"; do
	case "$param" in
		--help|-h)
			usage $0
			exit 0
			;;
		--work-dir=*) TEST_DIR="${param#*=}" ;;
		--os=*) os_image="${param#*=}" ;;
		--fio-bin=*) fio_bin="${param}" ;;
		-x) set -x ;;
		-v) SPDK_VHOST_VERBOSE=true	;;
		*)
			usage $0 "Invalid argument '$param'"
			exit 1;;
	esac
done

. $(readlink -e "$(dirname $0)/../common/common.sh") || exit 1

job_file="$BASE_DIR/migration-malloc.job"

trap 'error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT

function vm_monitor_send()
{
	local vm_num=$1
	local cmd_result_file="$2"
	local vm_dir="$VM_BASE_DIR/$1"
	local vm_monitor_port=$(cat $vm_dir/monitor_port)

	[[ ! -z "$vm_monitor_port" ]] || fail "No monitor port!"

	shift 2
	nc 127.0.0.1 $vm_monitor_port "$@" > $cmd_result_file
}

# Migrate VM $1
function vm_migrate()
{
	local from_vm_dir="$VM_BASE_DIR/$1"
	local target_vm_dir="$(readlink -e $from_vm_dir/vm_migrate_to)"
	local target_vm="$(basename $target_vm_dir)"
	local target_vm_migration_port="$(cat $target_vm_dir/migration_port)"

	# Sanity check if target VM (QEMU) is configured to accept source VM (QEMU) migration
	if [[ "$(readlink -e ${target_vm_dir}/vm_incoming)" != "$(readlink -e ${from_vm_dir})" ]]; then
		fail "source VM $1 or destination VM is not properly configured for live migration"
	fi

	notice "Migrating VM $1 to VM "$(basename $target_vm_dir)
	echo -e \
		"migrate_set_speed 1g\n" \
		"migrate tcp:127.0.0.1:$target_vm_migration_port\n" \
		"info migrate\n" \
		"quit" | vm_monitor_send $1 "$from_vm_dir/migration_result"

	# Post migration checks:
	if ! grep "Migration status: completed"  $from_vm_dir/migration_result -q; then
		cat $from_vm_dir/migration_result
		fail "Migration failed:\n"
	fi

	if ! vm_os_booted $target_vm; then
		fail "VM$target_vm is not running"
		cat $target_vm $target_vm_dir/cont_result
	fi

	notice "Migration complete"
}

# FIXME: this shoul'd not be needed
vm_kill_all

rpc="python $SPDK_BUILD_DIR/scripts/rpc.py -s $(get_vhost_dir)/rpc.sock"

# Use 2 VMs:
# incoming VM - the one we want to migrate
# targe VM - the one which will accept migration
incoming_vm=0
target_vm=1
incoming_vm_ctrlr=naa.Malloc0.$incoming_vm
target_vm_ctrlr=naa.Malloc0.$target_vm

vm_setup --os="$os_image" --force=$incoming_vm --disk-type=spdk_vhost_scsi --disks=Malloc0 --migrate-to=$target_vm
vm_setup --force=$target_vm --disk-type=spdk_vhost_scsi --disks=Malloc0 --incoming=$incoming_vm

spdk_vhost_run --conf-path=$BASE_DIR

notice "==============="
notice ""
notice "Setting up VMs"
notice ""

declare -A vm_ctrlr

function clean_vhost_config()
{
	notice "Removing vhost devices & controllers via RPC ..."
	# Delete bdev first to remove all LUNs and SCSI targets
	$rpc delete_bdev Malloc0

	# Delete controllers
	$rpc remove_vhost_controller $incoming_vm_ctrlr
	$rpc remove_vhost_controller $target_vm_ctrlr
}

function error_migration_clean()
{
	trap - SIGINT ERR EXIT
	set -x

	vm_kill_all
	clean_vhost_config
}

function is_fio_running()
{
	local shell_restore_x="$( [[ "$-" =~ x ]] && echo 'set -x' )"
	set +x

	if vm_ssh $1 'kill -0 $(cat /root/fio.pid)'; then
		local ret=0
	else
		local ret=1
	fi

	$shell_restore_x
	return $ret
}

trap 'error_migration_clean; error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT

# Construct shared Malloc Bdev
$rpc construct_malloc_bdev -b Malloc0 128 4096

# And two controllers - one for each VM. Both are using the same Malloc Bdev as LUN 0
$rpc construct_vhost_scsi_controller $incoming_vm_ctrlr
$rpc add_vhost_scsi_lun $incoming_vm_ctrlr 0 Malloc0

$rpc construct_vhost_scsi_controller $target_vm_ctrlr
$rpc add_vhost_scsi_lun $target_vm_ctrlr 0 Malloc0

# Run everything
vm_run $incoming_vm $target_vm

# Wait only for incoming VM, as target is waiting for migration
vm_wait_for_boot 600 $incoming_vm

# Run fio before migration
notice "Starting FIO"

vm_check_scsi_location $incoming_vm
run_fio $fio_bin --job-file="$job_file" --local --vm="${incoming_vm}$(printf ':/dev/%s' $SCSI_DISK)"

# Wait a while to let the FIO time to issue some IO
sleep 5

# Check if fio is still running before migration
if ! is_fio_running $incoming_vm; then
	vm_ssh $incoming_vm "cat /root/$(basename ${job_file}).out"
	error "FIO is not running before migration: process crashed or finished too early"
fi

vm_migrate $incoming_vm
sleep 3

# Check if fio is still running after migration
if ! is_fio_running $target_vm; then
	vm_ssh $target_vm "cat /root/$(basename ${job_file}).out"
	error "FIO is not running after migration: process crashed or finished too early"
fi

notice "Waiting for fio to finish"
timeout=40
while is_fio_running $target_vm; do
	sleep 1
	echo -n "."
	if (( timeout-- == 0 )); then
		error "timeout while waiting for FIO!"
	fi
done

notice "Fio result is:"
vm_ssh $target_vm "cat /root/$(basename ${job_file}).out"

notice "Migration DONE"


notice "Shutting down all VMs"
vm_shutdown_all
clean_vhost_config

notice "killing vhost app"
spdk_vhost_kill

notice "Migration Test SUCCESS"
notice "==============="

trap - SIGINT ERR EXIT
