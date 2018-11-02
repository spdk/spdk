#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
source $curdir/common.sh

fio_verify --filename=MalCache1:MalCache2 --spdk_conf=$curdir/fio-mallocs.conf
status=$?

exit $status
