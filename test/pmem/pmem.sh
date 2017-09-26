#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"

test_info=false
test_create=false
test_delete=false
test_construct_bdev=false
test_delete_bdev=false
test_all=true
test_all_get=false
default_pool_file=$TEST_DIR/test/pmem/pool_file

function usage()
{
    [[ ! -z $2 ]] && ( echo "$2"; echo ""; )
    echo "Shortcut script for automated RPC tests for PMEM"
    echo "For test details, check test_plan.md or"
    echo "https://review.gerrithub.io/#/c/377341/9/test/pmem/test_plan.md"
    echo
    echo "Usage: $(basename $1) [OPTIONS]"
    echo
    echo "-h, --help                Print help and exit"
    echo "-x                        set -x for script debug"
    echo "    --info                Run test cases for pmem_pool_info"
    echo "    --create              Run test cases for create_pmem_pool"
    echo "    --delete              Run test cases for delete_pmem_pool"
    echo "    --construct_bdev      Run test cases for constructing pmem bdevs"
    echo "    --delete_bdev         Run test cases for deleting pmem bdevs"
    echo "    --all                 Run all test cases (default)"
    exit 0
}

while getopts 'xh-:' optchar; do
    case "$optchar" in
        -)
        case "$OPTARG" in
            help) usage $0 ;;
            info) test_info=true; test_all=false;;
            create) test_create=true; test_all=false;;
            delete) test_delete=true; test_all=false;;
            construct_bdev) test_construct_bdeve=true; test_all=false;;
            delete_bdev) test_delete_bdev=true; test_all=false;;
            info) test_info=true; test_all=false;;
            all) test_all_get=true;;
            *) usage $0 "Invalid argument '$OPTARG'" ;;
        esac
        ;;
    h) usage $0 ;;
    x) set -x ;;
    *) usage $0 "Invalid argument '$OPTARG'"
    esac
done

if $test_all_get; then
	test_all=true
fi

if [[ $EUID -ne 0 ]]; then
	echo "Go away user come back as root"
	exit 1
fi

source $TEST_DIR/test/pmem/common/common.sh

#================================================
# pmem_pool_info tests
#================================================
function pmem_pool_info_tc1()
{
	if $rpc_py pmem_pool_info; then
		error "pmem_pool_info passed with missing argument!"
	fi
	
	return 0
}

function pmem_pool_info_tc2()
{
	if $rpc_py pmem_pool_info $TEST_DIR/non/existing/path/non_existent_file; then
		error "pmem_pool_info passed with invalid path!" $ENODEV
	fi
	
	return 0
}

function pmem_pool_info_tc3()
{
	if [ -f $TEST_DIR/test/pmem/obj_pool_file ]; then
		echo "Deleting old type OBJ pool_file"
		rm $TEST_DIR/test/pmem/obj_pool_file
	fi

	echo "Creating new OBJ pool file"
	if [ $(dpkg-query -W -f='${Status}' nvml-tools 2>/dev/null | grep -c "ok installed") -eq 1 ]; then
		pmempool create -s 32000000 obj $TEST_DIR/test/pmem/obj_pool_file
	else
		echo "Warning: nvml-tools package not found! Creating stub file."
		touch $TEST_DIR/test/pmem/obj_pool_file
		truncate -c -s 32M $TEST_DIR/test/pmem/obj_pool_file
	fi

	if [ ! -f $TEST_DIR/test/pmem/obj_pool_file ]; then
			error "Creating obj_pool_file failed!"
	fi

	if $rpc_py pmem_pool_info $TEST_DIR/test/pmem/obj_pool_file; then
		error "Pmem_pool_info passed with invalid pool_file type!" $ENODEV
	fi

	echo "Deleting type OBJ pool_file"
	rm $TEST_DIR/test/pmem/obj_pool_file
	return 0
}

function pmem_pool_info_tc4()
{
	if ! $rpc_py pmem_pool_info $default_pool_file; then
		error "Failed to get pmem_pool_info!"
	fi
	
	return 0
}

#================================================
# create_pmem_pool tests
#================================================
function create_pmem_pool_tc1()
{
	pmem_clean_pool_file

	if $rpc_py create_pmem_pool 32 512; then
		error "Mem pool file created w/out given path!"
	fi

	if $rpc_py pmem_pool_info $default_pool_file; then
		error "something, something, something...!"
	fi

	if $rpc_py create_pmem_pool $default_pool_file; then
		error "Mem pool file created w/out size & block size arguments!"
	fi

	if $rpc_py pmem_pool_info $default_pool_file; then
		error "something, something, something...!"
	fi

	if $rpc_py create_pmem_pool $default_pool_file 32; then
		error "Mem pool file created w/out block size argument!"
	fi

	if $rpc_py pmem_pool_info $default_pool_file; then
		error "something, something, something...!"
	fi

	pmem_clean_pool_file
	return 0
}

function create_pmem_pool_tc2()
{
	pmem_clean_pool_file

	if  $rpc_py create_pmem_pool $TEST_DIR/non/existing/path/non_existent_file 32 512; then
		error "Mem pool file created with incorrect path!"
	fi

	if $rpc_py pmem_pool_info $TEST_DIR/non/existing/path/non_existent_file; then
		error "something, something, something...!"
	fi

	pmem_clean_pool_file
	return 0
}

#TODO need to check info & compare with expected
function create_pmem_pool_tc3()
{
	pmem_clean_pool_file

	if ! $rpc_py create_pmem_pool $default_pool_file 256 512; then
		error "Failed to create pmem pool!"
	fi
	
	if ! $rpc_py pmem_pool_info $default_pool_file; then
		error "Failed to get pmem info"
	fi
	
	if $rpc_py delete_pmem_pool $default_pool_file; then
		echo "Success!"
		true
	fi
	
	if [ -f $TEST_DIR/test/pmem/pool_file ]; then
		echo "Fail!"
		false
	fi
	
	pmem_clean_pool_file
	return 0
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

#================================================
# delete_pmem_pool tests
#================================================
#TODO
function delete_pmem_pool_tc1()
{
	pmem_clean_pool_file

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
	pmem_clean_pool_file
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
	pmem_clean_pool_file
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
if $test_info || $test_all; then
	pmem_create_pool_file
	pmem_pool_info_tc1
	pmem_pool_info_tc2
	pmem_pool_info_tc3
	pmem_pool_info_tc4
	pmem_clean_pool_file
fi

if $test_create || $test_all; then
	create_pmem_pool_tc1
	create_pmem_pool_tc2
	create_pmem_pool_tc3
	create_pmem_pool_tc4
	create_pmem_pool_tc5
	create_pmem_pool_tc6
	create_pmem_pool_tc7
	create_pmem_pool_tc8
fi

if $test_delete || $test_all; then
	delete_pmem_pool_tc1
	delete_pmem_pool_tc2
	delete_pmem_pool_tc3
	delete_pmem_pool_tc4
fi

if $test_construct_bdev || $test_all; then
	construct_pmem_bdev_tc1
	construct_pmem_bdev_tc2
	construct_pmem_bdev_tc3
	construct_pmem_bdev_tc4
	construct_pmem_bdev_tc5
	construct_pmem_bdev_tc6
fi

if $test_delete_bdev || $test_all; then
	delete_bdev_tc1
	delete_bdev_tc2
fi

pmem_clean_pool_file
nbd_kill

