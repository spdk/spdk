#!/usr/bin/env bash

. $(readlink -f $(dirname $0))/common.sh || exit 1

# We handle error in this script so don't trap errors
trap - ERR

function usage()
{
	echo -e "
Shortcut script for shutting down VMs
Usage: $(basename $1) [OPTIONS] [VMs]

OPTIONS
       -a     kill/shutdown all running VMs
       -k     kill instead of shutdown
       --vm=N kill or shutdown VM N. Might be used several times to kill many VM at once.     
"
	spdk_vhost_usage_common
}

do_kill=false
all=false
vms_to_kill=()

for arg in ${VHOST_TEST_ARGS[@]}; do
	case "$arg" in
		-k) do_kill=true ;;
		-a) all=true ;;
		-h|--help) usage $0 ;;
		--vm=*) vms_to_kill+=( "${arg#*=}" ) ;;
		*)
			error "Invalid argument '$arg'"
			exit 1
		;;
	esac
done

if ! $all && (( ${#vms_to_kill[@]} == 0 )); then
	error "Use '-a' or '--vm=N' to do something."
	exit 1
elif $do_kill && [[ $EUID -ne 0 ]]; then
	error "Go away user come back as root"
	exit 1
fi

if $all; then
	if do_kill; then
		echo 'INFO: killing all VMs'
		vm_kill_all
	else
		echo 'INFO: shutting down all VMs'
		vm_shutdown_all
	fi
else
	if $do_kill; then
		echo "INFO: killing VMs: ${vms_to_kill[@]}"
		for vm in ${vms_to_kill[@]}; do
			if ! vm_kill $vm; then
				error "Failed to kill vm"
				exit 1
			fi
		done
	else
		echo 'INFO: shutting down all VMs'
		if ! vm_shutdown ${vms_to_kill[0]}; then
			error "Failed to shutdown VM ${vms_to_kill[0]}"
			exit 1
		fi
	fi
fi
