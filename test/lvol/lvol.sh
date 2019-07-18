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
    echo "    --test-cases=         List test cases which will be run:
                                    254: 'destroy_after_bdev_lvol_resize_positive',
                                    255: 'delete_lvol_store_persistent_positive',
                                    300: 'bdev_lvol_delete_lvstore_nonexistent_lvs_uuid',
                                    301: 'delete_lvol_store_underlying_bdev',
                                    350: 'nested_destroy_logical_volume_negative',
                                    400: 'nested_construct_logical_volume_positive',
                                    550: 'delete_bdev_positive',
                                    551: 'delete_lvol_bdev',
                                    552: 'bdev_lvol_delete_lvstore_with_clones',
                                    553: 'unregister_lvol_bdev',
                                    602: 'bdev_lvol_create_lvstore_with_all_clear_methods',
                                    650: 'thin_provisioning_check_space',
                                    651: 'thin_provisioning_read_empty_bdev',
                                    652: 'thin_provisioning_data_integrity_test',
                                    653: 'thin_provisioning_resize',
                                    654: 'thin_overprovisioning',
                                    655: 'thin_provisioning_filling_disks_less_than_lvs_size',
                                    700: 'tasting_positive',
                                    701: 'tasting_lvol_store_positive',
                                    702: 'tasting_positive_with_different_lvol_store_cluster_size',
                                    750: 'snapshot_readonly',
                                    751: 'snapshot_compare_with_lvol_bdev',
                                    752: 'snapshot_during_io_traffic',
                                    753: 'snapshot_of_snapshot',
                                    754: 'clone_bdev_only',
                                    755: 'clone_writing_clone',
                                    756: 'clone_and_snapshot_consistency',
                                    757: 'clone_inflate',
                                    758: 'clone_decouple_parent',
                                    759: 'clone_decouple_parent_rw',
                                    760: 'set_read_only',
                                    761: 'delete_snapshot',
                                    762: 'delete_snapshot_with_snapshot',
                                    800: 'rename_positive',
                                    801: 'rename_lvs_nonexistent',
                                    802: 'rename_lvs_EEXIST',
                                    803: 'bdev_lvol_rename_nonexistent',
                                    804: 'bdev_lvol_rename_EEXIST',
                                    850: 'clear_method_none',
                                    851: 'clear_method_unmap',
                                    10000: 'SIGTERM'
                                    or
                                    all: This parameter runs all tests
                                    Ex: \"1,2,19,20\", default: all"
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
