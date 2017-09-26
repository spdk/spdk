#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"
rpc_py="$TEST_DIR/scripts/rpc.py "
RPC_PORT=5260

source $TEST_DIR/scripts/autotest_common.sh

ENODEV=19

# check if there is pool file & remove it
# input: path to pool file
function pmem_clean_pool_file()
{
	#default value
	local pool_file=$TEST_DIR/test/pmem/pool_file
	if [ ! -z "$1" ]; then
		pool_file=$1
	fi
	
	if [ -f $pool_file ]; then
		echo "Deleting old pool_file"
		rm $pool_file
	fi
}

# create new pmem file
# input: path to pool file, size in MB, block_size
function pmem_create_pool_file()
{
	#default values
	local pool_file=$TEST_DIR/test/pmem/pool_file
	local size=32
	local block_size=512
	
	if [ ! -z "$1" ]; then
		pool_file=$1
	fi
	
	if [ ! -z "$2" ]; then
		size=$2
	fi
	
	if [ ! -z "$3" ]; then
		block_size=$3
	fi
	
	pmem_clean_pool_file $pool_file
	echo "Creating new pool file"
	$rpc_py create_pmem_pool $pool_file $size $block_size
	if [ ! -f $pool_file ]; then
		echo "Creating pool_file failed!"
		false
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
		echo "No nbd pid file found!"
		false
	fi

    waitforlisten $nbd_pid $RPC_PORT
    # delete Malloc0 as it's no longer needed
    $rpc_py delete_bdev Malloc0
}

function nbd_kill()
{
	if [[ ! -f $TEST_DIR/test/pmem/nbd.pid ]]; then
		echo "No nbd pid file found!"
		false
	fi

    if pkill -F $TEST_DIR/test/pmem/nbd.pid; then
        sleep 1
        rm $TEST_DIR/test/pmem/nbd.pid
    fi

    rmmod nbd
}

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
