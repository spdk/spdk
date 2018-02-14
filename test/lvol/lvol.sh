#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"

total_size=64
block_size=512
cluster_sz=1048576 #1MiB
test_cases=all
x=""

rpc_py="$TEST_DIR/scripts/rpc.py "

function usage() {
    [[ ! -z $2 ]] && ( echo "$2"; echo ""; )
    echo "Shortcut script for doing automated lvol tests"
    echo "Usage: $(basename $1) [OPTIONS]"
    echo
    echo "-h, --help                print help and exit"
    echo "    --total-size          Size of malloc bdev in MB (int > 0)"
    echo "    --block-size          Block size for this bdev"
    echo "    --cluster-sz          size of cluster (in bytes)"
    echo "-x                        set -x for script debug"
    echo "    --test-cases=         List test cases which will be run:
                                    1: 'construct_lvs_positive',
                                    50: 'construct_logical_volume_positive',
                                    51: 'construct_multi_logical_volumes_positive',
                                    52: 'construct_lvol_bdev_using_name_positive',
                                    53: 'construct_lvol_bdev_duplicate_names_positive',
                                    100: 'construct_logical_volume_nonexistent_lvs_uuid',
                                    101: 'construct_lvol_bdev_on_full_lvol_store',
                                    102: 'construct_lvol_bdev_name_twice',
                                    150: 'resize_lvol_bdev_positive',
                                    200: 'resize_logical_volume_nonexistent_logical_volume',
                                    201: 'resize_logical_volume_with_size_out_of_range',
                                    250: 'destroy_lvol_store_positive',
                                    251: 'destroy_lvol_store_use_name_positive',
                                    252: 'destroy_lvol_store_with_lvol_bdev_positive',
                                    253: 'destroy_multi_logical_volumes_positive',
                                    254: 'destroy_after_resize_lvol_bdev_positive',
                                    255: 'delete_lvol_store_persistent_positive',
                                    300: 'destroy_lvol_store_nonexistent_lvs_uuid',
                                    301: 'delete_lvol_store_underlying_bdev',
                                    350: 'nested_destroy_logical_volume_negative',
                                    400: 'nested_construct_logical_volume_positive',
                                    450: 'construct_lvs_nonexistent_bdev',
                                    451: 'construct_lvs_on_bdev_twice',
                                    452: 'construct_lvs_name_twice',
                                    500: 'nested_construct_lvol_bdev_on_full_lvol_store',
                                    550: 'delete_bdev_positive',
                                    600: 'construct_lvol_store_with_cluster_size_max',
                                    601 'construct_lvol_store_with_cluster_size_min',
                                    650: 'thin_provisioning_check_space',
                                    651: 'thin_provisioning_read_empty_bdev',
                                    652: 'thin_provisionind_data_integrity_test',
                                    653: 'thin_provisioning_resize',
                                    654: 'thin_overprovisioning',
                                    655: 'thin_provisioning_filling_disks_less_than_lvs_size',
                                    700: 'tasting_positive',
                                    701: 'tasting_lvol_store_positive',
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
            cluster-sz=*) cluster_sz="${OPTARG#*=}" ;;
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

source $TEST_DIR/scripts/autotest_common.sh

###  Function starts bdev_svc app
function bdev_svc_start()
{
    modprobe nbd
    touch $BASE_DIR/bdev_svc.conf
    $TEST_DIR/scripts/gen_nvme.sh >> $BASE_DIR/bdev_svc.conf
    $TEST_DIR/test/app/bdev_svc/bdev_svc -c $BASE_DIR/bdev_svc.conf &
    bdev_svc_pid=$!
    echo $bdev_svc_pid > $BASE_DIR/bdev_svc.pid
    waitforlisten $bdev_svc_pid
}

###  Function stops bdev_svc app
function bdev_svc_kill()
{
    ### Kill with SIGKILL param
    if pkill -F $BASE_DIR/bdev_svc.pid; then
        sleep 1
    fi
    rm $BASE_DIR/bdev_svc.pid || true
    rm $BASE_DIR/bdev_svc.conf || true
    rmmod nbd || true
}

trap "bdev_svc_kill; exit 1" SIGINT SIGTERM EXIT

bdev_svc_start
$BASE_DIR/lvol_test.py $rpc_py $total_size $block_size $cluster_sz $BASE_DIR $TEST_DIR/test/app/bdev_svc "${test_cases[@]}"

bdev_svc_kill
trap - SIGINT SIGTERM EXIT
