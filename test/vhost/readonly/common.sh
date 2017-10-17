#!/usr/bin/env bash
set +x
BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"
rpc_py="$TEST_DIR/scripts/rpc.py "
RPC_PORT=5260

source $TEST_DIR/../scripts/autotest_common.sh
source $TEST_DIR/vhost/common/common.sh

function vhost_start()
{
	spdk_vhost_run --conf-dir=$BASE_DIR
}

function vhost_kill()
{
	spdk_vhost_kill
}

function print_tc_name()
{
	echo ""
	echo "==============================================================="
	echo "Now running: $1"
	echo "==============================================================="
}
