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
	nbd_kill
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
	$rpc_py create_pmem_pool $pool_file $size $block_size
	if [ ! -f $pool_file ]; then
		error "Creating pool_file failed!"
	fi
}

function pmem_create_obj_pool_file()
{
	local obj_pool_file=${1:-$TEST_DIR/test/pmem/pool_file}
	local size=${2:-32}
	local size_forpmempol=$size*1000000
	
	echo "Creating new type OBJ pool file"
	if [ $(dpkg-query -W -f='${Status}' nvml-tools 2>/dev/null | grep -c "ok installed") -eq 0 ]; then
		pmempool create -s $size_forpmempol obj $TEST_DIR/test/pmem/obj_pool_file
	else
		echo "Warning: nvml-tools package not found! Creating stub file."
		touch $obj_pool_file
		truncate -c -s $size"M" $obj_pool_file
	fi

	if [ ! -f $obj_pool_file ]; then
			error "Creating obj_pool_file failed!"
	fi
}

function nbd_start()
{

    [[ -f $TEST_DIR/test/pmem/nbd.pid ]] && nbd_kill
    modprobe nbd
    # add Malloc0 bdev as it is required to start the nbd app
    $TEST_DIR/test/lib/bdev/nbd/nbd -c $TEST_DIR/test/pmem/nbd.conf.in \
    -b Malloc0 -n /dev/nbd0 &
    nbd_pid=$!
    echo $nbd_pid > $TEST_DIR/test/pmem/nbd.pid
    if [[ ! -f $TEST_DIR/test/pmem/nbd.pid ]]; then
		error "No nbd pid file found!"
	fi

    waitforlisten $nbd_pid $RPC_PORT
    # delete Malloc0 as it's no longer needed
    $rpc_py delete_bdev Malloc0
}

function nbd_kill()
{
	if [[ ! -f $TEST_DIR/test/pmem/nbd.pid ]]; then
		error "No nbd pid file found!"
	fi

    if pkill -F $TEST_DIR/test/pmem/nbd.pid; then
        sleep 1
        rm $TEST_DIR/test/pmem/nbd.pid
    fi

    rmmod nbd
}

