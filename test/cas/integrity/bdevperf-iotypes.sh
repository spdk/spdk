#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
source $curdir/common.sh

bdevperf -c $curdir/fio-mallocs.conf -q 128 -o 4096 -t 4 -w flush
