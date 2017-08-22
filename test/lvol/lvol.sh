#!/usr/bin/env bash
set -e
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"

x=""

function usage()
{
	[[ ! -z $2 ]] && ( echo "$2"; echo ""; )
	echo "Shortcut script for doing automated test"
	echo "Usage: $(basename $1) [OPTIONS]"
	echo
	echo "-h, --help                print help and exit"
	exit 0
}

function vhost_start()
{
$TEST_DIR/app/vhost/vhost -c $BASE_DIR/vhost.conf.in -f $BASE_DIR/vhost.pid &
sleep 5
}

function  vhost_kill()
{
pkill -f $BASE_DIR/vhost.pid>>/dev/null
rm $BASE_DIR/vhost.pid>>/dev/null
}

rpc_py="$TEST_DIR/scripts/rpc.py "
for total_size in 64 128 32; do
    for block_size in 512 4096; do
        vhost_start
        ./lvol_test.py $rpc_py $total_size $block_size
        vhost_kill
    done
done







