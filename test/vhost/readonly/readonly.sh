#!/usr/bin/env bash
set +x

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"

if [[ $EUID -ne 0 ]]; then
	echo "Go away user come back as root"
	exit 1
fi

source $TEST_DIR/vhost/readonly/common.sh

function blk_ro_tc1()
{
	print_tc_name ${FUNCNAME[0]}
}

function blk_ro_tc2()
{
	print_tc_name ${FUNCNAME[0]}
}

vhost_run
blk_ro_tc1
blk_ro_tc2
vhost_kill
