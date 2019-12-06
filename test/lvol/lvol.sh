#!/usr/bin/env bash

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

total_size=256
block_size=512
test_cases=all
x=""

rpc_py="$rootdir/scripts/rpc.py "

function usage() {
	[[ -n $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for doing automated lvol tests"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "-h, --help                print help and exit"
	echo "    --total-size          Size of malloc bdev in MB (int > 0)"
	echo "    --block-size          Block size for this bdev"
	echo "-x                        set -x for script debug"
	echo "    --test-cases=         List test cases which will be run:"
	echo "                          150: 'bdev_lvol_resize_positive',"
	echo "                          200: 'resize_logical_volume_nonexistent_logical_volume',"
	echo "                          201: 'resize_logical_volume_with_size_out_of_range',"
	echo "                          250: 'bdev_lvol_delete_lvstore_positive',"
	echo "                          251: 'bdev_lvol_delete_lvstore_use_name_positive',"
	echo "                          252: 'bdev_lvol_delete_lvstore_with_lvol_bdev_positive',"
	echo "                          253: 'destroy_multi_logical_volumes_positive',"
	echo "                          254: 'destroy_after_bdev_lvol_resize_positive',"
	echo "                          255: 'delete_lvol_store_persistent_positive',"
	echo "                          300: 'bdev_lvol_delete_lvstore_nonexistent_lvs_uuid',"
	echo "                          301: 'delete_lvol_store_underlying_bdev',"
	echo "                          350: 'nested_destroy_logical_volume_negative',"
	echo "                          400: 'nested_construct_logical_volume_positive',"
	echo "                          450: 'construct_lvs_nonexistent_bdev',"
	echo "                          451: 'construct_lvs_on_bdev_twice',"
	echo "                          452: 'construct_lvs_name_twice',"
	echo "                          500: 'nested_bdev_lvol_create_on_full_lvol_store',"
	echo "                          550: 'delete_bdev_positive',"
	echo "                          551: 'delete_lvol_bdev',"
	echo "                          552: 'bdev_lvol_delete_lvstore_with_clones',"
	echo "                          553: 'unregister_lvol_bdev',"
	echo "                          600: 'bdev_lvol_create_lvstore_with_cluster_size_max',"
	echo "                          601: 'bdev_lvol_create_lvstore_with_cluster_size_min',"
	echo "                          602: 'bdev_lvol_create_lvstore_with_all_clear_methods',"
	echo "                          650: 'thin_provisioning_check_space',"
	echo "                          651: 'thin_provisioning_read_empty_bdev',"
	echo "                          652: 'thin_provisioning_data_integrity_test',"
	echo "                          653: 'thin_provisioning_resize',"
	echo "                          654: 'thin_overprovisioning',"
	echo "                          655: 'thin_provisioning_filling_disks_less_than_lvs_size',"
	echo "                          700: 'tasting_positive',"
	echo "                          701: 'tasting_lvol_store_positive',"
	echo "                          702: 'tasting_positive_with_different_lvol_store_cluster_size',"
	echo "                          750: 'snapshot_readonly',"
	echo "                          751: 'snapshot_compare_with_lvol_bdev',"
	echo "                          752: 'snapshot_during_io_traffic',"
	echo "                          753: 'snapshot_of_snapshot',"
	echo "                          754: 'clone_bdev_only',"
	echo "                          755: 'clone_writing_clone',"
	echo "                          756: 'clone_and_snapshot_consistency',"
	echo "                          757: 'clone_inflate',"
	echo "                          758: 'clone_decouple_parent',"
	echo "                          759: 'clone_decouple_parent_rw',"
	echo "                          760: 'set_read_only',"
	echo "                          761: 'delete_snapshot',"
	echo "                          762: 'delete_snapshot_with_snapshot',"
	echo "                          800: 'rename_positive',"
	echo "                          801: 'rename_lvs_nonexistent',"
	echo "                          802: 'rename_lvs_EEXIST',"
	echo "                          803: 'bdev_lvol_rename_nonexistent',"
	echo "                          804: 'bdev_lvol_rename_EEXIST',"
	echo "                          10000: 'SIGTERM'"
	echo "                          or"
	echo "                          all: This parameter runs all tests"
	echo "                          Ex: \"1,2,19,20\", default: all"
	echo
	echo
}

while getopts 'xh-:' optchar; do
	case "$optchar" in
		-)
		case "$OPTARG" in
			help) usage $0 && exit 0;;
			total-size=*) total_size="${OPTARG#*=}" ;;
			block-size=*) block_size="${OPTARG#*=}" ;;
			test-cases=*) test_cases="${OPTARG#*=}" ;;
			*) usage $0 "Invalid argument '$OPTARG'" && exit 1 ;;
		esac
		;;
	h) usage $0 && exit 0 ;;
	x) set -x
		x="-x" ;;
	*) usage $0 "Invalid argument '$OPTARG'" && exit 1 ;;
	esac
done
shift $(( OPTIND - 1 ))

###  Function starts vhost app
function vhost_start()
{
	modprobe nbd
	$rootdir/app/vhost/vhost &
	vhost_pid=$!
	echo $vhost_pid > $testdir/vhost.pid
	waitforlisten $vhost_pid
}

###  Function stops vhost app
function vhost_kill()
{
	### Kill with SIGKILL param
	if pkill -F $testdir/vhost.pid; then
		sleep 1
	fi
	rm $testdir/vhost.pid || true
}

trap 'vhost_kill; rm -f $testdir/aio_bdev_0 $testdir/aio_bdev_1; exit 1' SIGINT SIGTERM EXIT

truncate -s 400M $testdir/aio_bdev_0 $testdir/aio_bdev_1
vhost_start
$testdir/lvol_test.py $rpc_py $total_size $block_size $testdir $rootdir/app/vhost "${test_cases[@]}"

vhost_kill 0
rm -rf $testdir/aio_bdev_0 $testdir/aio_bdev_1
trap - SIGINT SIGTERM EXIT
