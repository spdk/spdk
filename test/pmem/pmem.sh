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
	if [ -f $TEST_DIR/test/pmem/obj_pool_file ]; then
		echo "Deleting old type OBJ pool_file"
		rm $TEST_DIR/test/pmem/obj_pool_file
	fi

	echo "Creating new type OBJ pool file"
	if [ $(dpkg-query -W -f='${Status}' nvml-tools 2>/dev/null | grep -c "ok installed") -eq 1 ]; then
		pmempool create -s 32000000 obj $TEST_DIR/test/pmem/obj_pool_file
	else
		echo "Warning: nvml-tools package not found! Creating stub file."
		touch $TEST_DIR/test/pmem/obj_pool_file
		truncate -c -s 32M $TEST_DIR/test/pmem/obj_pool_file
	fi

	if [ ! -f $TEST_DIR/test/pmem/obj_pool_file ]; then
			echo "Creating obj_pool_file failed!"
			false
	fi

	if $rpc_py pmem_pool_info $TEST_DIR/test/pmem/obj_pool_file; then
		echo "pmem_pool_info passed with invalid pool_file type!"
		false
	fi

	echo "Deleting type OBJ pool_file"
	rm $TEST_DIR/test/pmem/obj_pool_file
}

function pmem_pool_info_tc4()
{
if ! $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
	echo "Failed to get pmem_pool_info!"
	false
fi
}
#### create_pmem_pool_tc1
function create_pmem_pool_tc1()
{
	if [ -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Deleting old pool_file"
		rm $TEST_DIR/test/pmem/pool_file
	fi
	#w/out path argument

	if $rpc_py create_pmem_pool 32 512; then
		echo "Fail!"
		false
	fi

	if $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Fail!"
		false
	fi

	#w/out size & block size arguments
	if $rpc_py create_pmem_pool $TEST_DIR/test/pmem/pool_file; then
		echo "Fail!"
		false
	fi

	if $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Fail!"
		false
	fi

	#w/out block size arguments
	if $rpc_py create_pmem_pool $TEST_DIR/test/pmem/pool_file 32; then
		echo "Fail!"
		false
	fi

	if $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Fail!"
		false
	fi
}

#### create_pmem_pool_tc2
function create_pmem_pool_tc2()
{
	if [ -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Deleting old pool_file"
		rm $TEST_DIR/test/pmem/pool_file
	fi

	#invalid path
	if  $rpc_py create_pmem_pool $TEST_DIR/non/existing/path/non_existent_file 32 512; then
	echo "Fail!"
	false
	fi
	if $rpc_py pmem_pool_info $TEST_DIR/non/existing/path/non_existent_file; then
		echo "Fail!"
		false
	fi
}

#### create_pmem_pool_tc3
function create_pmem_pool_tc3()
{
	if [ -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Deleting old pool_file"
		rm $TEST_DIR/test/pmem/pool_file
	fi
	if $rpc_py create_pmem_pool $TEST_DIR/test/pmem/pool_file 256 512; then
		echo "Success!"
		true
	fi
	if $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		true
	fi
	if $rpc_py delete_pmem_pool $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		true
	fi
	if [ -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Fail!"
		false
	fi

}

#### create_pmem_pool_tc4
function create_pmem_pool_tc4()
{
		echo "tbd"
}

#### create_pmem_pool_tc5
function create_pmem_pool_tc5()
{
	#TODO: compare size and block size after calling create_pmem_pool
	if [ -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Deleting old pool_file"
		rm $TEST_DIR/test/pmem/pool_file
	fi
	if $rpc_py create_pmem_pool $TEST_DIR/test/pmem/pool_file 256 512; then
		echo "Success!"
		true
	fi
	if $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		true
	fi
	if $rpc_py create_pmem_pool $TEST_DIR/test/pmem/pool_file 512 4096; then
		echo "Fail!"
		false
	fi
	if $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		true
	fi
	if $rpc_py delete_pmem_pool $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		true
	fi
}

#### create_pmem_pool_tc6
#this needs to be re-done
function create_pmem_pool_tc6()
{
	#TODO: compare size and block size after calling create_pmem_pool
	if [ -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Deleting old pool_file"
		rm $TEST_DIR/test/pmem/pool_file
	fi
	if $rpc_py create_pmem_pool $TEST_DIR/test/pmem/pool_file 256 -1; then
		echo "Success!"
		true
	fi
	if $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		true
	fi
	if $rpc_py delete_pmem_pool $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		true
	fi
	if $rpc_py create_pmem_pool $TEST_DIR/test/pmem/pool_file 256 511; then
		echo "Success!"
		true
	fi
	if $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		true
	fi
	if $rpc_py delete_pmem_pool $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		true
	fi
	if $rpc_py create_pmem_pool $TEST_DIR/test/pmem/pool_file 256 512; then
		echo "Success!"
		true
	fi
	if $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		true
	fi
	if $rpc_py delete_pmem_pool $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		true
	fi
}

#### create_pmem_pool_tc7
function create_pmem_pool_tc7()
{
	if [ -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Deleting old pool_file"
		rm $TEST_DIR/test/pmem/pool_file
	fi
	if $rpc_py create_pmem_pool $TEST_DIR/test/pmem/pool_file 15 512; then
		echo "Success!"
		true
	fi
	if  $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Fail!"
		false
	fi
}

#### create_pmem_pool_tc8
#This one fails?
function create_pmem_pool_tc8()
{
	if [ -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Deleting old pool_file"
		rm $TEST_DIR/test/pmem/pool_file
	fi
	if ! $rpc_py create_pmem_pool $TEST_DIR/test/pmem/pool_file 30 128; then
		echo "Success!"
		true
	fi
	if  $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Fail!"
		#false
	fi
}
#### delete_pmem_pool_tc1
function delete_pmem_pool_tc1()
{
	if [ -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Deleting old pool_file"
		rm $TEST_DIR/test/pmem/pool_file
	fi
	if $rpc_py delete_pmem_pool $TEST_DIR/test/pmem/pool_file; then
		echo "Fail!"
		false
	fi
}

#### delete_pmem_pool_tc2
function delete_pmem_pool_tc2()
{
	if [ -f $TEST_DIR/test/pmem/obj_pool_file ]; then
		echo "Deleting old type OBJ pool_file"
		rm $TEST_DIR/test/pmem/obj_pool_file
	fi

	echo "Creating new type OBJ pool file"
	if [ $(dpkg-query -W -f='${Status}' nvml-tools 2>/dev/null | grep -c "ok installed") -eq 1 ]; then
		pmempool create -s 32000000 obj $TEST_DIR/test/pmem/obj_pool_file
	else
		echo "Warning: nvml-tools package not found! Creating stub file."
		touch $TEST_DIR/test/pmem/obj_pool_file
		truncate -c -s 32M $TEST_DIR/test/pmem/obj_pool_file
	fi

	if [ ! -f $TEST_DIR/test/pmem/obj_pool_file ]; then
			echo "Creating obj_pool_file failed!"
			false
	fi
	if $rpc_py delete_pmem_pool $TEST_DIR/test/pmem/obj_pool_file; then
		echo "Fail!"
		false
	fi
	if [ -f $TEST_DIR/test/pmem/obj_pool_file ]; then
		echo "Deleting old type OBJ pool_file"
		rm $TEST_DIR/test/pmem/obj_pool_file
	fi
}

#### delete_pmem_pool_tc3
function delete_pmem_pool_tc3()
{
	if [ -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Deleting old pool_file"
		rm $TEST_DIR/test/pmem/pool_file
	fi
	if $rpc_py create_pmem_pool $TEST_DIR/test/pmem/pool_file 256 512; then
		echo "Success!"
		true
	fi
	if $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		true
	fi
	if $rpc_py delete_pmem_pool $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		true
	fi
	if $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Fail!"
		false
	fi
}

#### delete_pmem_pool_tc4
function delete_pmem_pool_tc4()
{
	delete_pmem_pool_tc3
	if $rpc_py delete_pmem_pool $TEST_DIR/test/pmem/pool_file; then
		echo "Fail!"
		false
	fi
}

# construct_pmem_bdev_tc1
function construct_pmem_bdev_tc1()
{
	pmem_create_pool_file
	if $rpc_py construct_pmem_bdev; then
		echo "Fail!"
		false
	fi
	pmem_remove_pool_file
}

# construct_pmem_bdev_tc2
# needs to add get_bdevs bdev extraction & compare
function construct_pmem_bdev_tc2()
{
	pmem_create_pool_file
	if $rpc_py construct_pmem_bdev $TEST_DIR/non/existing/path/non_existent_file; then
		echo "Fail!"
		false
	fi
	# tbd
	$rpc_py get_bdevs
	pmem_remove_pool_file
}

# construct_pmem_bdev_tc3
function construct_pmem_bdev_tc3()
{
	touch $TEST_DIR/test/pmem/random_file
	truncate -c -s 32M $TEST_DIR/test/pmem/random_file
	if [ ! -f $TEST_DIR/test/pmem/random_file ]; then
			echo "Creating random_file failed!"
			false
	fi
	if $rpc_py construct_pmem_bdev $TEST_DIR/test/pmem/random_file; then
		echo "Fail!"
		false
	fi
	if [ -f $TEST_DIR/test/pmem/random_file ]; then
		echo "Deleting previously created random file"
		rm $TEST_DIR/test/pmem/random_file
	fi
}

# construct_pmem_bdev_tc4
function construct_pmem_bdev_tc4()
{
	if [ -f $TEST_DIR/test/pmem/obj_pool_file ]; then
		echo "Deleting old type OBJ pool_file"
		rm $TEST_DIR/test/pmem/obj_pool_file
	fi

	echo "Creating new type OBJ pool file"
	if [ $(dpkg-query -W -f='${Status}' nvml-tools 2>/dev/null | grep -c "ok installed") -eq 1 ]; then
		pmempool create -s 32000000 obj $TEST_DIR/test/pmem/obj_pool_file
	else
		echo "Warning: nvml-tools package not found! Creating stub file."
		touch $TEST_DIR/test/pmem/obj_pool_file
		truncate -c -s 32M $TEST_DIR/test/pmem/obj_pool_file
	fi
	if $rpc_py construct_pmem_bdev $TEST_DIR/test/pmem/obj_pool_file; then
		echo "Fail!"
		false
	fi
	if [ -f $TEST_DIR/test/pmem/obj_pool_file ]; then
		echo "Deleting type OBJ pool_file"
		rm $TEST_DIR/test/pmem/obj_pool_file
	fi
}

# construct_pmem_bdev_tc5
# this test fails?
function construct_pmem_bdev_tc5()
{
	if [ -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Deleting old pool_file"
		rm $TEST_DIR/test/pmem/pool_file
	fi
	if $rpc_py create_pmem_pool $TEST_DIR/test/pmem/pool_file 32 512; then
		echo "Success!"
		true
	fi
	if $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		true
	fi
	if ! $rpc_py construct_pmem_bdev $TEST_DIR/test/pmem/pool_file; then
		echo "Fail!"
		false
	fi
	# tbd
	$rpc_py get_bdevs
	# tbd
	if ! $rpc_py delete_bdev pmem0; then
		echo "Fail!"
#		false
	fi
	if $rpc_py delete_pmem_pool $TEST_DIR/test/pmem/pool_file; then
		echo "Fail!"
#		false
	fi
}

#### construct_pmem_bdev_tc6
function construct_pmem_bdev_tc6()
{
	if [ -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Deleting old pool_file"
		rm $TEST_DIR/test/pmem/pool_file
	fi
	if $rpc_py create_pmem_pool $TEST_DIR/test/pmem/pool_file 32 512; then
		echo "Success!"
		true
	fi
	if $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		true
	fi
	if ! $rpc_py construct_pmem_bdev $TEST_DIR/test/pmem/pool_file; then
		echo "Fail!"
		false
	fi
	# tbd
	$rpc_py get_bdevs
	if  $rpc_py construct_pmem_bdev $TEST_DIR/test/pmem/pool_file; then
		echo "Fail!"
		false
	fi
	# tbd
	if ! $rpc_py delete_bdev pmem0; then
		echo "Fail!"
#		false
	fi
	if $rpc_py delete_pmem_pool $TEST_DIR/test/pmem/pool_file; then
		echo "Fail!"
		false
	fi
}

#### delete_bdev_tc1
function delete_bdev_tc1()
{
	if [ -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Deleting old pool_file"
		rm $TEST_DIR/test/pmem/pool_file
	fi
	if ! $rpc_py construct_malloc_bdev 32 512; then
		echo "Fail!"
		false
	fi
	#tbd
#	$rpc_py construct_aio_bdev;
	#tbd
#	$rpc_py construct_nvme_bdev;
	if ! $rpc_py create_pmem_pool $TEST_DIR/test/pmem/pool_file 256 512; then
		echo "Success!"
		false
	fi
	if ! $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		false
	fi
	if ! $rpc_py construct_pmem_bdev $TEST_DIR/test/pmem/pool_file; then
		echo "Fail!"
		false
	fi
	# tbd
	$rpc_py get_bdevs
	if ! $rpc_py delete_bdev pmem0; then
		echo "Fail!"
#		false
	fi
	# tbd
	$rpc_py get_bdevs
}

#### delete_bdev_tc2
function delete_bdev_tc2()
{
	if [ -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Deleting old pool_file"
		rm $TEST_DIR/test/pmem/pool_file
	fi
	if ! $rpc_py create_pmem_pool $TEST_DIR/test/pmem/pool_file 256 512; then
		echo "Success!"
		false
	fi
	if ! $rpc_py pmem_pool_info $TEST_DIR/test/pmem/pool_file; then
		echo "Success!"
		false
	fi
	if ! $rpc_py construct_pmem_bdev $TEST_DIR/test/pmem/pool_file; then
		echo "Fail!"
		false
	fi
	# tbd
	$rpc_py get_bdevs
	if ! $rpc_py delete_bdev pmem0; then
		echo "Fail!"
#		false
	fi
	# tbd
	$rpc_py get_bdevs
	if $rpc_py delete_bdev pmem0; then
		echo "Fail!"
#		false
	fi
}

nbd_start
pmem_create_pool_file
pmem_pool_info_tc4
pmem_pool_info_tc1
pmem_pool_info_tc2
pmem_pool_info_tc3
create_pmem_pool_tc1
create_pmem_pool_tc2
create_pmem_pool_tc3
create_pmem_pool_tc4
create_pmem_pool_tc5
create_pmem_pool_tc6
create_pmem_pool_tc7
create_pmem_pool_tc8
delete_pmem_pool_tc1
delete_pmem_pool_tc2
delete_pmem_pool_tc3
delete_pmem_pool_tc4
construct_pmem_bdev_tc1
construct_pmem_bdev_tc2
construct_pmem_bdev_tc3
construct_pmem_bdev_tc4
construct_pmem_bdev_tc5
construct_pmem_bdev_tc6
delete_bdev_tc1
delete_bdev_tc2
pmem_remove_pool_file
nbd_kill
