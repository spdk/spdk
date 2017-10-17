#!/usr/bin/env bash

BASE_DIR=$(readlink -f $(dirname $0))
[[ -z "$TEST_DIR" ]] && TEST_DIR="$(cd $BASE_DIR/../../ && pwd)"

if [[ $EUID -ne 0 ]]; then
	echo "Go away user come back as root"
	exit 1
fi

source $TEST_DIR/test/vhot/readonly/common.sh

function blk_ro_tc1
{
}

function blk_ro_tc2
{
}