#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin

source $rootdir/test/common/autotest_common.sh

function fio_verify(){
	LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio $curdir/test.fio --aux-path=/tmp/ --ioengine=spdk_bdev $@
}

fio_verify --filename=MalCache1:MalCache2 --spdk_conf=$curdir/mallocs.conf
