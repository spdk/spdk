#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh

# By default use Guest fio
test_cases=""
function usage()
{
	[[ -n $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for doing automated test of live migration."
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "    --test-cases=TESTS    Coma-separated list of tests to run. Implemented test cases are: 1"
	echo "                          See test/vhost/test_plan.md for more info."
	echo "-x                        set -x for script debug"
}

for param in "$@"; do
	case "$param" in
		--help|-h)
			usage $0
			exit 0
			;;
		--test-cases=*) test_cases="${param#*=}" ;;
		-x) set -x ;;
		*)
			usage $0 "Invalid argument '$param'"
			exit 1;;
	esac
done

vhosttestinit

[[ -n "$test_cases" ]] || fail "Need '--test-cases=' parameter"

trap 'error_exit "${FUNCNAME}" "${LINENO}"' INT ERR EXIT

function vm_monitor_send()
{
	local cmd_result_file="$2"
	local vm_dir="$VM_DIR/$1"
	local vm_monitor_port
	vm_monitor_port=$(cat $vm_dir/monitor_port)

	[[ -n "$vm_monitor_port" ]] || fail "No monitor port!"

	shift 2
	nc 127.0.0.1 $vm_monitor_port "$@" > $cmd_result_file
}

# Migrate VM $1
function vm_migrate()
{
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
	if ! grep "Migration status: completed"  $from_vm_dir/migration_result -q; then
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

function is_fio_running()
{
	local shell_restore_x
	shell_restore_x="$( [[ "$-" =~ x ]] && echo 'set -x' )"
	set +x

	if vm_exec $1 'kill -0 $(cat /root/fio.pid)'; then
		local ret=0
	else
		local ret=1
	fi

	$shell_restore_x
	return $ret
}

for test_case in ${test_cases//,/ }; do
	assert_number "$test_case"
	notice "==============================="
	notice "Running Migration test case ${test_case}"
	notice "==============================="

	timing_enter migration-tc${test_case}
	source $testdir/migration-tc${test_case}.sh
	timing_exit migration-tc${test_case}
done

notice "Migration Test SUCCESS"
notice "==============="

trap - SIGINT ERR EXIT

vhosttestfini
