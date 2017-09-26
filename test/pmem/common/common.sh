#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"
rpc_py="$TEST_DIR/scripts/rpc.py "
RPC_PORT=5260

source $TEST_DIR/scripts/autotest_common.sh

ENODEV=19
ENOENT=2
ENOTBLK=15

# Prints error message and return error code, closes nbd app and remove
# pmem pool file
# input: error message, error code
function error()
{
	local error_code=${2:-1}
	echo "==========="
	echo -e "ERROR: $1"
	echo "error code: $error_code"
	echo "==========="
	vhost_kill
	pmem_clean_pool_file
	return $error_code
}

# check if there is pool file & remove it
# input: path to pool file
# default: $TEST_DIR/test/pmem/pool_file
function pmem_clean_pool_file()
{
	local pool_file=${1:-$TEST_DIR/test/pmem/pool_file}

	if [ -f $pool_file ]; then
		echo "Deleting old pool_file"
		rm $pool_file
	fi
}

# create new pmem file
# input: path to pool file, size in MB, block_size
# default: $TEST_DIR/test/pmem/pool_file 32 512
function pmem_create_pool_file()
{
	local pool_file=${1:-$TEST_DIR/test/pmem/pool_file}
	local size=${2:-32}
	local block_size=${3:-512}

	pmem_clean_pool_file $pool_file
	echo "Creating new pool file"
	if ! $rpc_py create_pmem_pool $pool_file $size $block_size; then
		error "Creating pool_file failed!"
	fi

	if [ ! -f $pool_file ]; then
		error "Creating pool_file failed!"
	fi
}

# create new OBJ type pmem file
# input: path to pool file, size in MB, block_size
# default: $TEST_DIR/test/pmem/pool_file 32 512
function pmem_create_obj_pool_file()
{
	local obj_pool_file=${1:-$TEST_DIR/test/pmem/obj_pool_file}
	local size=${2:-32}
	local size_for_pmempol=$(( $size*1000000 ))

	echo "Creating new type OBJ pool file"
	if [[ -f /usr/bin/pmempool ]]; then
		pmempool create -s $size_for_pmempol obj $obj_pool_file
	else
		echo "Warning: nvml-tools package not found! Creating stub file."
		touch $obj_pool_file
		truncate -c -s $size"M" $obj_pool_file
	fi

	if [ ! -f $obj_pool_file ]; then
			error "Creating obj_pool_file failed!"
	fi
}

function pmem_unmount_ramspace
{
	if [ -d "$TEST_DIR/test/pmem/ramspace" ]; then
		umount $TEST_DIR/test/pmem/ramspace
		rm -rf $TEST_DIR/test/pmem/ramspace
	fi
}

function pmem_print_tc_name
{
	echo "=================================="
	echo "Now running: $1"
	echo "=================================="
}

function vhost_start()
{
	local vhost_reactor_mask=${1:-0x1}
	local vhost_master_core=${2:-0}
	local vhost_conf_template="$TEST_DIR/test/pmem/vhost.conf.in"
	local vhost_conf_file="$TEST_DIR/test/pmem/vhost.conf"
	[[ -f $TEST_DIR/test/pmem/vhost.pid ]] && vhost_kill

	cp $vhost_conf_template $vhost_conf_file
	$BASE_DIR/../../scripts/gen_nvme.sh >> $vhost_conf_file

	$TEST_DIR/app/vhost/vhost -m $vhost_reactor_mask -p $vhost_master_core -c $vhost_conf_file &
	vhost_pid=$!
	echo $vhost_pid > $TEST_DIR/test/pmem/vhost.pid
	if [[ ! -f $TEST_DIR/test/pmem/vhost.pid ]]; then
		error "No vhost pid file found!"
	fi

	waitforlisten $vhost_pid $RPC_PORT
	rm $vhost_conf_file
}

function vhost_kill()
{
	local vhost_pid_file="$TEST_DIR/test/pmem/vhost.pid"

	if [[ ! -f $TEST_DIR/test/pmem/vhost.pid ]]; then
		error "No vhost pid file found!"
	fi

	echo "INFO: killing vhost (PID $vhost_pid) app"
	local vhost_pid="$(cat $vhost_pid_file)"

	if /bin/kill -INT $vhost_pid >/dev/null; then
		echo "INFO: sent SIGINT to vhost app - waiting 60 seconds to exit"
		for ((i=0; i<60; i++)); do
			if /bin/kill -0 $vhost_pid; then
				echo "."
				sleep 1
			else
				break
			fi
		done
		if /bin/kill -0 $vhost_pid; then
			echo "ERROR: vhost was NOT killed - sending SIGKILL"
			/bin/kill -KILL $vhost_pid
			rm $vhost_pid_file
			return 1
		fi
	elif /bin/kill -0 $vhost_pid; then
		error "vhost NOT killed - you need to kill it manually"
		return 1
	else
		echo "INFO: vhost was no running"
	fi

	rm $vhost_pid_file
}
