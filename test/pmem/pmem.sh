#!/usr/bin/env bash

set -xe

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"

source $TEST_DIR/test/pmem/common/common.sh

function pmem_create_pool_file()
{
	if [ ! -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Creating new pool file"
	else
		echo "Deleting old pool_file"
		rm $TEST_DIR/test/pmem/pool_file
	fi

	$rpc_py create_pmem_pool $TEST_DIR/test/pmem/pool_file 32 512
	if [ ! -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Creating pool_file failed!"
		false
	fi
}

function pmem_remove_pool_file()
{
if [ -f $TEST_DIR/test/pmem/pool_file ]; then
	echo "Deleting pool_file"
	rm $TEST_DIR/test/pmem/pool_file
	if [ -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Removing pool_file failed!"
		false
	fi
fi
}

## pmem_pool_info_tc1
function pmem_pool_info_tc1()
{
if  $rpc_py pmem_pool_info; then
	echo "pmem_pool_info passed with missing argument!"
	false
fi
}

## pmem_pool_info_tc2
function pmem_pool_info_tc2()
{
if  $rpc_py pmem_pool_info $TEST_DIR/non/existing/path/non_existent_file; then
	echo "pmem_pool_info passed with invalid path!"
	false
fi
}

## pmem_pool_info_tc3
function pmem_pool_info_tc3()
{
}

function pmem_pool_info_tc4()
{
if ! $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
	echo "Failed to get pmem_pool_info!"
	false
fi
}

nbd_start
pmem_create_pool_file
pmem_pool_info_tc4
pmem_pool_info_tc1
pmem_pool_info_tc2
pmem_remove_pool_file
nbd_kill
