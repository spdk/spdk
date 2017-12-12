#!/usr/bin/env bash
set -xe
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../../../ && pwd)"
APP_FILE=$TEST_DIR/test/app/bdev_svc/bdev_svc
CONF_FILE=${TEST_DIR}/test/lib/bdev/bdev.conf
PID_FILE=$BASE_DIR/nbd.pid

test_cases=all
x=""

rpc_py="$TEST_DIR/scripts/rpc.py "

function usage() {
    [[ ! -z $2 ]] && ( echo "$2"; echo ""; )
    echo "Shortcut script for doing automated nbd tests"
    echo "Usage: $(basename $1) [OPTIONS]"
    echo
    echo "-h, --help                print help and exit"
    echo "-x                        set -x for script debug"
    echo "    --test-cases=         List test cases which will be run:
                                    1: 'start_nbd_disk_positive',
                                    2: 'list_nbd_disks_one_by_one_positive',
                                    3: 'list_all_nbd_disks_positive',
                                    4: 'stop_nbd_disk_positive',
                                    or
                                    all: This parameter runs all tests
                                    Ex: \"1,3,4\", default: all"
    echo
    echo
    exit 0
}

while getopts 'xh-:' optchar; do
    case "$optchar" in
        -)
        case "$OPTARG" in
            help) usage $0 ;;
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

###  Function starts nbd app
function nbd_start()
{
    $APP_FILE -c $CONF_FILE &
    nbd_pid=$!
    echo $nbd_pid > $PID_FILE
    waitforlisten $nbd_pid
}

###  Function stops nbd app
function nbd_kill()
{
    if [ ! -f $PID_FILE ]; then
        return
    fi
    ### Kill with SIGKILL param
    nbd_pid=`cat $PID_FILE`
    if pkill -F $PID_FILE; then
        ### Timeout waiting for current app completely exit
        for i in {1..30}; do
        	if kill -s 0 $nbd_pid; then
                echo "Info: Terminating SPDK APP..."
                sleep 1
            else
                break
            fi
        done
        ### Timeout waiting for nbd thread completely exit
        for ((i=1; i<=20; i++)); do
		    if grep -q nbd /proc/partitions; then
			    sleep 0.1
		    else
		    	break
		    fi
	    done
    fi
    rm $PID_FILE || true
}

trap "nbd_kill; exit 1" SIGINT SIGTERM EXIT

nbd_start
$BASE_DIR/nbd_test.py $rpc_py $PID_FILE $APP_FILE $CONF_FILE "${test_cases[@]}"

nbd_kill
trap - SIGINT SIGTERM EXIT
