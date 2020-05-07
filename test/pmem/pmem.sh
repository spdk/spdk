#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/pmem/common.sh

rpc_py="$rootdir/scripts/rpc.py "

enable_script_debug=false
test_info=false
test_create=false
test_delete=false
test_construct_bdev=false
test_delete_bdev=false
test_all=true
test_all_get=false
default_pool_file="$testdir/pool_file"
obj_pool_file="$testdir/obj_pool_file"
bdev_name=pmem0

function usage() {
	[[ -n $2 ]] && (
		echo "$2"
		echo ""
	)
	echo "Shortcut script for automated RPC tests for PMEM"
	echo
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "-h, --help                Print help and exit"
	echo "-x                        set -x for script debug"
	echo "    --info                Run test cases for bdev_pmem_get_pool_info"
	echo "    --create              Run test cases for bdev_pmem_create_pool"
	echo "    --delete              Run test cases for bdev_pmem_delete_pool"
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
				info)
					test_info=true
					test_all=false
					;;
				create)
					test_create=true
					test_all=false
					;;
				delete)
					test_delete=true
					test_all=false
					;;
				construct_bdev)
					test_construct_bdev=true
					test_all=false
					;;
				delete_bdev)
					test_delete_bdev=true
					test_all=false
					;;
				all) test_all_get=true ;;
				*) usage $0 "Invalid argument '$OPTARG'" ;;
			esac
			;;
		h) usage $0 ;;
		x) enable_script_debug=true ;;
		*) usage $0 "Invalid argument '$OPTARG'" ;;
	esac
done

if $test_all_get; then
	test_all=true
fi

if [[ $EUID -ne 0 ]]; then
	echo "Go away user come back as root"
	exit 1
fi

#================================================
# bdev_pmem_get_pool_info tests
#================================================
function bdev_pmem_get_pool_info_tc1() {
	pmem_print_tc_name ${FUNCNAME[0]}

	if $rpc_py bdev_pmem_get_pool_info; then
		error "bdev_pmem_get_pool_info passed with missing argument!"
	fi

	return 0
}

function bdev_pmem_get_pool_info_tc2() {
	pmem_print_tc_name ${FUNCNAME[0]}

	if $rpc_py bdev_pmem_get_pool_info $rootdir/non/existing/path/non_existent_file; then
		error "bdev_pmem_get_pool_info passed with invalid path!"
	fi

	return 0
}

function bdev_pmem_get_pool_info_tc3() {
	pmem_print_tc_name ${FUNCNAME[0]}
	pmem_clean_pool_file $obj_pool_file

	echo "Creating new type OBJ pool file"
	if hash pmempool; then
		pmempool create -s 32000000 obj $obj_pool_file
	else
		echo "Warning: pmempool package not found! Creating stub file."
		truncate -s "32M" $obj_pool_file
	fi

	if $rpc_py bdev_pmem_get_pool_info $obj_pool_file; then
		pmem_clean_pool_file $obj_pool_file
		error "Pmem_pool_info passed with invalid pool_file type!"
	fi

	pmem_clean_pool_file $obj_pool_file
	return 0
}

function bdev_pmem_get_pool_info_tc4() {
	pmem_print_tc_name ${FUNCNAME[0]}
	pmem_clean_pool_file

	pmem_create_pool_file
	if ! $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		error "Failed to get bdev_pmem_get_pool_info!"
	fi

	pmem_clean_pool_file
	return 0
}

#================================================
# bdev_pmem_create_pool tests
#================================================
function bdev_pmem_create_pool_tc1() {
	pmem_print_tc_name ${FUNCNAME[0]}
	pmem_clean_pool_file

	if $rpc_py bdev_pmem_create_pool 32 512; then
		error "Mem pool file created w/out given path!"
	fi

	if $rpc_py bdev_pmem_create_pool $default_pool_file; then
		error "Mem pool file created w/out size & block size arguments!"
	fi

	if $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		error "bdev_pmem_create_pool created invalid pool file!"
	fi

	if $rpc_py bdev_pmem_create_pool $default_pool_file 32; then
		error "Mem pool file created w/out block size argument!"
	fi

	if $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		error "bdev_pmem_create_pool created invalid pool file!"
	fi

	pmem_clean_pool_file
	return 0
}

function bdev_pmem_create_pool_tc2() {
	pmem_print_tc_name ${FUNCNAME[0]}
	pmem_clean_pool_file

	if $rpc_py bdev_pmem_create_pool $rootdir/non/existing/path/non_existent_file 32 512; then
		error "Mem pool file created with incorrect path!"
	fi

	if $rpc_py bdev_pmem_get_pool_info $rootdir/non/existing/path/non_existent_file; then
		error "bdev_pmem_create_pool created invalid pool file!"
	fi

	pmem_clean_pool_file
	return 0
}

function bdev_pmem_create_pool_tc3() {
	pmem_print_tc_name ${FUNCNAME[0]}
	pmem_clean_pool_file

	if ! $rpc_py bdev_pmem_create_pool $default_pool_file 256 512; then
		error "Failed to create pmem pool!"
	fi

	if ! $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		error "Failed to get pmem info"
	fi

	if ! $rpc_py bdev_pmem_delete_pool $default_pool_file; then
		error "Failed to delete pool file!"
	fi

	if $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		error "Got pmem file info but file should be deleted"
	fi

	if [ -f $default_pool_file ]; then
		error "Failed to delete pmem file!"
	fi

	pmem_clean_pool_file
	return 0
}

function bdev_pmem_create_pool_tc4() {
	pmem_print_tc_name ${FUNCNAME[0]}

	pmem_unmount_ramspace
	mkdir $rootdir/test/pmem/ramspace
	mount -t tmpfs -o size=300m tmpfs $rootdir/test/pmem/ramspace
	if ! $rpc_py bdev_pmem_create_pool $rootdir/test/pmem/ramspace/pool_file 256 512; then
		pmem_unmount_ramspace
		error "Failed to create pmem pool!"
	fi

	if ! $rpc_py bdev_pmem_get_pool_info $rootdir/test/pmem/ramspace/pool_file; then
		pmem_unmount_ramspace
		error "Failed to get pmem info"
	fi

	if ! $rpc_py bdev_pmem_delete_pool $rootdir/test/pmem/ramspace/pool_file; then
		pmem_unmount_ramspace
		error "Failed to delete pool file!"
	fi

	if [ -f $rootdir/test/pmem/ramspace/pool_file ]; then
		pmem_unmount_ramspace
		error "Failed to delete pmem file / file still exists!"
	fi

	pmem_unmount_ramspace
	return 0
}

function bdev_pmem_create_pool_tc5() {
	pmem_print_tc_name ${FUNCNAME[0]}
	pmem_clean_pool_file
	local pmem_block_size
	local pmem_num_block

	if ! $rpc_py bdev_pmem_create_pool $default_pool_file 256 512; then
		error "Failed to create pmem pool!"
	fi

	if $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		pmem_block_size=$($rpc_py bdev_pmem_get_pool_info $default_pool_file | jq -r '.[] .block_size')
		pmem_num_block=$($rpc_py bdev_pmem_get_pool_info $default_pool_file | jq -r '.[] .num_blocks')
	else
		error "Failed to get pmem info!"
	fi

	if $rpc_py bdev_pmem_create_pool $default_pool_file 512 4096; then
		error "Pmem pool with already occupied path has been created!"
	fi

	if $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		if [ $pmem_block_size != $($rpc_py bdev_pmem_get_pool_info $default_pool_file | jq -r '.[] .block_size') ]; then
			error "Invalid block size of pmem pool!"
		fi

		if [ $pmem_num_block != $($rpc_py bdev_pmem_get_pool_info $default_pool_file | jq -r '.[] .num_blocks') ]; then
			error "Invalid number of blocks of pmem pool!"
		fi
	else
		error "Failed to get pmem info!"
	fi

	if ! $rpc_py bdev_pmem_delete_pool $default_pool_file; then
		error "Failed to delete pmem file!"
	fi

	pmem_clean_pool_file
	return 0
}

function bdev_pmem_create_pool_tc6() {
	pmem_print_tc_name ${FUNCNAME[0]}
	pmem_clean_pool_file
	local created_pmem_block_size

	for i in 511 512 1024 2048 4096 131072 262144; do
		if ! $rpc_py bdev_pmem_create_pool $default_pool_file 256 $i; then
			error "Failed to create pmem pool!"
		fi

		if ! created_pmem_block_size=$($rpc_py bdev_pmem_get_pool_info $default_pool_file | jq -r '.[] .block_size'); then
			error "Failed to get pmem info!"
		fi

		if [ $i != $created_pmem_block_size ]; then
			error "Invalid block size of pmem pool!"
		fi

		if ! $rpc_py bdev_pmem_delete_pool $default_pool_file; then
			error "Failed to delete pmem file!"
		fi
	done

	pmem_clean_pool_file
	return 0
}

function bdev_pmem_create_pool_tc7() {
	pmem_print_tc_name ${FUNCNAME[0]}
	pmem_clean_pool_file

	if $rpc_py bdev_pmem_create_pool $default_pool_file 15 512; then
		error "Created pmem pool with invalid size!"
	fi

	if $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		error "Pmem file shouldn' exist!"
	fi

	pmem_clean_pool_file
	return 0
}

function bdev_pmem_create_pool_tc8() {
	pmem_print_tc_name "bdev_pmem_create_pool_tc8"
	pmem_clean_pool_file

	if $rpc_py bdev_pmem_create_pool $default_pool_file 32 65536; then
		error "Created pmem pool with invalid block number!"
	fi

	if $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		error "Pmem file shouldn' exist!"
	fi

	pmem_clean_pool_file
	return 0
}

function bdev_pmem_create_pool_tc9() {
	pmem_print_tc_name ${FUNCNAME[0]}
	pmem_clean_pool_file

	if $rpc_py bdev_pmem_create_pool $default_pool_file 256 -1; then
		error "Created pmem pool with negative block size number!"
	fi

	if $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		error "bdev_pmem_create_pool create invalid pool file!"
	fi

	if $rpc_py bdev_pmem_create_pool $default_pool_file -1 512; then
		error "Created pmem pool with negative size number!"
	fi

	if $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		error "bdev_pmem_create_pool create invalid pool file!"
	fi

	pmem_clean_pool_file
	return 0
}

#================================================
# bdev_pmem_delete_pool tests
#================================================
function bdev_pmem_delete_pool_tc1() {
	pmem_print_tc_name ${FUNCNAME[0]}
	pmem_clean_pool_file

	if $rpc_py bdev_pmem_delete_pool $default_pool_file; then
		error "bdev_pmem_delete_pool deleted inexistant pool file!"
	fi

	return 0
}

function bdev_pmem_delete_pool_tc2() {
	pmem_print_tc_name "bdev_pmem_delete_pool_tc2"
	pmem_clean_pool_file $obj_pool_file

	echo "Creating new type OBJ pool file"
	if hash pmempool; then
		pmempool create -s 32000000 obj $obj_pool_file
	else
		echo "Warning: pmempool package not found! Creating stub file."
		truncate -s "32M" $obj_pool_file
	fi

	if $rpc_py bdev_pmem_delete_pool $obj_pool_file; then
		pmem_clean_pool_file $obj_pool_file
		error "bdev_pmem_delete_pool deleted invalid pmem pool type!"
	fi

	pmem_clean_pool_file $obj_pool_file
	return 0
}

function bdev_pmem_delete_pool_tc3() {
	pmem_print_tc_name ${FUNCNAME[0]}
	pmem_clean_pool_file

	pmem_create_pool_file
	if ! $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		error "Failed to get info on pmem pool file!"
	fi

	if ! $rpc_py bdev_pmem_delete_pool $default_pool_file; then
		error "Failed to delete pmem pool file!"
	fi

	if $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		error "Pmem pool file exists after using bdev_pmem_get_pool_info!"
	fi

	return 0
}

function bdev_pmem_delete_pool_tc4() {
	pmem_print_tc_name ${FUNCNAME[0]}

	bdev_pmem_delete_pool_tc3
	if $rpc_py bdev_pmem_delete_pool $default_pool_file; then
		error "Deleted pmem pool file that shouldn't exist!"
	fi

	return 0
}

#================================================
# bdev_pmem_create tests
#================================================
function bdev_pmem_create_tc1() {
	pmem_print_tc_name ${FUNCNAME[0]}
	pmem_clean_pool_file

	pmem_create_pool_file
	if $rpc_py bdev_pmem_create; then
		error "bdev_pmem_create passed with missing argument!"
	fi

	pmem_clean_pool_file
	return 0
}

function bdev_pmem_create_tc2() {
	pmem_print_tc_name ${FUNCNAME[0]}
	pmem_clean_pool_file

	pmem_create_pool_file
	if $rpc_py bdev_pmem_create -n $bdev_name $rootdir/non/existing/path/non_existent_file; then
		error "Created pmem bdev w/out valid pool file!"
	fi

	if $rpc_py bdev_get_bdevs | jq -r '.[] .name' | grep -qi pmem; then
		error "bdev_pmem_create passed with invalid argument!"
	fi

	pmem_clean_pool_file
	return 0
}

function bdev_pmem_create_tc3() {
	pmem_print_tc_name ${FUNCNAME[0]}

	truncate -s 32M $rootdir/test/pmem/random_file
	if $rpc_py bdev_pmem_create -n $bdev_name $rootdir/test/pmem/random_file; then
		error "Created pmem bdev from random file!"
	fi

	if [ -f $rootdir/test/pmem/random_file ]; then
		echo "Deleting previously created random file"
		rm $rootdir/test/pmem/random_file
	fi

	return 0
}

function bdev_pmem_create_tc4() {
	pmem_print_tc_name ${FUNCNAME[0]}
	pmem_clean_pool_file $obj_pool_file

	echo "Creating new type OBJ pool file"
	if hash pmempool; then
		pmempool create -s 32000000 obj $obj_pool_file
	else
		echo "Warning: pmempool package not found! Creating stub file."
		truncate -s "32M" $obj_pool_file
	fi

	if $rpc_py bdev_pmem_create -n $bdev_name $obj_pool_file; then
		pmem_clean_pool_file $obj_pool_file
		error "Created pmem bdev from obj type pmem file!"
	fi

	pmem_clean_pool_file $obj_pool_file
	return 0
}

function bdev_pmem_create_tc5() {
	pmem_print_tc_name ${FUNCNAME[0]}
	pmem_clean_pool_file
	pmem_create_pool_file
	local pmem_bdev_name

	if ! $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		error "Failed to get pmem info!"
	fi

	if ! pmem_bdev_name=$($rpc_py bdev_pmem_create -n $bdev_name $default_pool_file); then
		error "Failed to create pmem bdev"
	fi

	if ! $rpc_py bdev_get_bdevs | jq -r '.[] .name' | grep -qi $pmem_bdev_name; then
		error "Pmem bdev not found!"
	fi

	if ! $rpc_py bdev_pmem_delete $pmem_bdev_name; then
		error "Failed to delete pmem bdev!"
	fi

	if ! $rpc_py bdev_pmem_delete_pool $default_pool_file; then
		error "Failed to delete pmem pool file!"
	fi

	pmem_clean_pool_file
	return 0
}

function bdev_pmem_create_tc6() {
	pmem_print_tc_name ${FUNCNAME[0]}
	local pmem_bdev_name
	pmem_clean_pool_file

	pmem_create_pool_file
	if ! $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		error "Failed to get info on pmem pool file!"
	fi

	if ! pmem_bdev_name=$($rpc_py bdev_pmem_create -n $bdev_name $default_pool_file); then
		error "Failed to create pmem bdev!"
	fi

	if ! $rpc_py bdev_get_bdevs | jq -r '.[] .name' | grep -qi $pmem_bdev_name; then
		error "Pmem bdev not found!"
	fi

	if $rpc_py bdev_pmem_create -n $bdev_name $default_pool_file; then
		error "Constructed pmem bdev with occupied path!"
	fi

	if ! $rpc_py bdev_pmem_delete $pmem_bdev_name; then
		error "Failed to delete pmem bdev!"
	fi

	if ! $rpc_py bdev_pmem_delete_pool $default_pool_file; then
		error "Failed to delete pmem pool file!"
	fi

	pmem_clean_pool_file
	return 0
}

#================================================
# bdev_pmem_delete tests
#================================================
function delete_bdev_tc1() {
	pmem_print_tc_name ${FUNCNAME[0]}
	local pmem_bdev_name
	local bdevs_names
	pmem_clean_pool_file

	pmem_create_pool_file $default_pool_file 256 512
	if ! $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		error "Failed to get pmem info!"
	fi

	if ! pmem_bdev_name=$($rpc_py bdev_pmem_create -n $bdev_name $default_pool_file); then
		error "Failed to create pmem bdev!"
	fi

	if ! $rpc_py bdev_get_bdevs | jq -r '.[] .name' | grep -qi $pmem_bdev_name; then
		error "$pmem_bdev_name bdev not found!"
	fi

	if ! $rpc_py bdev_pmem_delete $pmem_bdev_name; then
		error "Failed to delete $pmem_bdev_name bdev!"
	fi

	bdevs_names=$($rpc_py bdev_get_bdevs | jq -r '.[] .name')
	if echo $bdevs_names | grep -qi $pmem_bdev_name; then
		error "$pmem_bdev_name bdev is not deleted!"
	fi

	pmem_clean_pool_file
	return 0
}

function delete_bdev_tc2() {
	pmem_print_tc_name ${FUNCNAME[0]}
	pmem_clean_pool_file
	pmem_create_pool_file $default_pool_file 256 512
	local pmem_bdev_name

	if ! $rpc_py bdev_pmem_get_pool_info $default_pool_file; then
		error "Failed to get pmem info!"
	fi

	if ! pmem_bdev_name=$($rpc_py bdev_pmem_create -n $bdev_name $default_pool_file); then
		error "Failed to create pmem bdev"
	fi

	if ! $rpc_py bdev_get_bdevs | jq -r '.[] .name' | grep -qi $pmem_bdev_name; then
		error "Pmem bdev not found!"
	fi

	if ! $rpc_py bdev_pmem_delete $pmem_bdev_name; then
		error "Failed to delete pmem bdev!"
	fi

	if $rpc_py bdev_pmem_delete $pmem_bdev_name; then
		error "bdev_pmem_delete deleted pmem bdev for second time!"
	fi

	pmem_clean_pool_file
	return 0
}

vhost_start
if ! $enable_script_debug; then
	set +x
fi

if $test_info || $test_all; then
	bdev_pmem_get_pool_info_tc1
	bdev_pmem_get_pool_info_tc2
	bdev_pmem_get_pool_info_tc3
	bdev_pmem_get_pool_info_tc4
fi

if $test_create || $test_all; then
	bdev_pmem_create_pool_tc1
	bdev_pmem_create_pool_tc2
	bdev_pmem_create_pool_tc3
	bdev_pmem_create_pool_tc4
	bdev_pmem_create_pool_tc5
	bdev_pmem_create_pool_tc6
	bdev_pmem_create_pool_tc7
	bdev_pmem_create_pool_tc8
	bdev_pmem_create_pool_tc9
fi

if $test_delete || $test_all; then
	bdev_pmem_delete_pool_tc1
	bdev_pmem_delete_pool_tc2
	bdev_pmem_delete_pool_tc3
	bdev_pmem_delete_pool_tc4
fi

if $test_construct_bdev || $test_all; then
	bdev_pmem_create_tc1
	bdev_pmem_create_tc2
	bdev_pmem_create_tc3
	bdev_pmem_create_tc4
	bdev_pmem_create_tc5
	bdev_pmem_create_tc6
fi

if $test_delete_bdev || $test_all; then
	delete_bdev_tc1
	delete_bdev_tc2
fi

pmem_clean_pool_file
vhost_kill 0
