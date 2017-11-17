#!/usr/bin/env bash
set -xe
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
                                    2: 'construct_logical_volume_positive',
                                    3: 'construct_multi_logical_volumes_positive',
                                    4: 'resize_lvol_bdev_positive',
                                    5: 'destroy_lvol_store_positive',
                                    6: 'destroy_lvol_store_with_lvol_bdev_positive',
                                    7: 'destroy_multi_logical_volumes_positive',
                                    8: 'nested construct_logical_volume_positive',
                                    9: 'destroy_after_resize_lvol_bdev_positive',
                                    10: 'construct_lvs_nonexistent_bdev',
                                    11: 'construct_lvs_on_bdev_twice_negative',
                                    12: 'construct_logical_volume_nonexistent_lvs_uuid',
                                    13: 'construct_logical_volumes_on_busy_bdev',
                                    14: 'resize_logical_volume_nonexistent_logical_volume',
                                    15: 'resize_logical_volume_with_size_out_of_range',
                                    16: 'destroy_lvol_store_nonexistent_lvs_uuid',
                                    17: 'destroy_lvol_store_nonexistent_bdev',
                                    18: 'nested construct_logical_volume_on_busy_bdev',
                                    19: 'nested destroy_logical_volume_positive',
                                    20: 'delete_bdev_positive',
                                    21: 'construct_lvs_with_cluster_sz_out_of_range_max',
                                    22: 'construct_lvs_with_cluster_sz_out_of_range_min',
                                    23: 'tasting_positive',
                                    24: 'SIGTERM'
                                    or
                                    all: This parameter runs all tests
                                    Ex: \"1,2,19,20\", default: all"
    echo
    echo
    exit 0
}

while getopts 'xh-:' optchar; do
    case "$optchar" in
        -)
        case "$OPTARG" in
            help) usage $0 ;;
            total-size=*) total_size="${OPTARG#*=}" ;;
            block-size=*) block_size="${OPTARG#*=}" ;;
            cluster-sz=*) cluster_sz="${OPTARG#*=}" ;;
            test-cases=*) test_cases="${OPTARG#*=}" ;;
            *) usage $0 "Invalid argument '$OPTARG'" ;;
        esac
        ;;
    h) usage $0 ;;
    x) set -x
        x="-x" ;;
    *) usage $0 "Invalid argument '$OPTARG'"
    esac
done
shift $(( OPTIND - 1 ))

source $TEST_DIR/scripts/autotest_common.sh

###  Function starts vhost app
function vhost_start()
{
    touch $BASE_DIR/vhost.conf
    $TEST_DIR/scripts/gen_nvme.sh >> $BASE_DIR/vhost.conf
    $TEST_DIR/app/vhost/vhost -c $BASE_DIR/vhost.conf &
    vhost_pid=$!
    echo $vhost_pid > $BASE_DIR/vhost.pid
    waitforlisten $vhost_pid
}

###  Function stops vhost app
function vhost_kill()
{
    ### Kill with SIGKILL param
    if pkill -F $BASE_DIR/vhost.pid; then
        sleep 1
    fi
    rm $BASE_DIR/vhost.pid || true
    rm $BASE_DIR/vhost.conf || true
}

trap "vhost_kill; exit 1" SIGINT SIGTERM EXIT

vhost_start
$BASE_DIR/lvol_test.py $rpc_py $total_size $block_size $cluster_sz $BASE_DIR $TEST_DIR/app/vhost "${test_cases[@]}"

vhost_kill
trap - SIGINT SIGTERM EXIT
