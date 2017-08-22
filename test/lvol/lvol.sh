#!/usr/bin/env bash
set -xe
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"

total_size=64
block_size=512
test_cases=all
x=""

function usage() {
    [[ ! -z $2 ]] && ( echo "$2"; echo ""; )
    echo "Shortcut script for doing automated lvol tests"
    echo "Usage: $(basename $1) [OPTIONS]"
    echo
    echo "-h, --help                print help and exit"
    echo "    --total-size          Size of malloc bdev in MB (int > 0)"
    echo "    --block-size          Block size for this bdev"
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
                                    11: 'construct_lvs_on_bdev_twic_negative',
                                    12: 'construct_logical_volume_nonexistent_lvs_uuid',
                                    13: 'construct_logical_volumes_on_busy_bdev',
                                    14: 'resize_logical_volume_nonexistent_logical_volume',
                                    15: 'resize_logical_volume_with_size_out_of_range',
                                    16: 'destroy_lvol_store_nonexistent_lvs_uuid',
                                    17: 'destroy_lvol_store_nonexistent_bdev',
                                    18: 'nested construct_logical_volume__on_busy_bdev',
                                    19: 'nested destroy_logical_volume_positive',
                                    20: 'delete_bdev_positive',
                                    21: 'nasted construct_logical_volume_positive',
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
RPC_PORT=5260

###  Function starts bdev_io app
function bdev_io_start()
{
    $TEST_DIR/examples/bdev/io/bdev_io -c $BASE_DIR/bdev_io.conf.in &
    bdev_io_pid=$!
    echo $bdev_io_pid > $BASE_DIR/bdev_io.pid
    waitforlisten $bdev_io_pid $RPC_PORT
}

###  Function stops vhost bdev_io app
function bdev_io_kill()
{
    bdev_io_pid="$(cat $BASE_DIR/bdev_io.pid)"
    ### Kill with SIGKILL param
    kill -KILL $bdev_io_pid
    sleep 5
    rm $BASE_DIR/bdev_io.pid
}

rpc_py="$TEST_DIR/scripts/rpc.py "

bdev_io_start
$BASE_DIR/lvol_test.py $rpc_py $total_size $block_size "${test_cases[@]}"
bdev_io_kill
