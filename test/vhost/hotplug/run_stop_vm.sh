#!/bin/bash -xe
set -e

HOTPLUG_DIR=$(readlink -f $(dirname $0))
. $HOTPLUG_DIR/common.sh


trap 'error_exit "${FUNCNAME}" "${LINENO}"' ERR

vms_setup_and_run "0"

counter=1
if [ $RUN_NIGHTLY -eq 1 ]; then
	counter = 5
fi
for ((i=1;i<=$counter;i++)); do
	sleep 1
	vm_kill "0"
        vm_run_with_arg "0"
done

vm_kill "0"
