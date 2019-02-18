#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"

source $TEST_DIR/test/common/autotest_common.sh

###  Function starts app
function tgt_start()
{
    $TEST_DIR/app/iscsi_tgt/iscsi_tgt &
    tgt_pid=$!
    echo $tgt_pid > $BASE_DIR/tgt.pid
    waitforlisten $tgt_pid
}

###  Function stops app
function tgt_kill()
{
    ### Kill with SIGKILL param
    if pkill -F $BASE_DIR/tgt.pid; then
        sleep 1
    fi
    rm $BASE_DIR/tgt.pid || true
}

trap "tgt_kill; exit 1" SIGINT SIGTERM EXIT

tgt_start

PYTHONPATH=$PYTHONPATH:$TEST_DIR/scripts/ $BASE_DIR/notify.py

tgt_kill

trap - SIGINT SIGTERM EXIT
