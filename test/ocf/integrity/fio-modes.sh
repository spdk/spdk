#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin

source $rootdir/test/common/autotest_common.sh

function fio_verify(){
	LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio $curdir/test.fio --aux-path=/tmp/ --ioengine=spdk_bdev $@
}

function cleanup(){
	rm -f $curdir/aio0 $curdir/aio1
	rm -f $curdir/modes.conf
}

truncate -s 101M $curdir/aio0
truncate -s 101M $curdir/aio1

trap "cleanup; exit 1" SIGINT SIGTERM EXIT

echo "
[Malloc]
  NumberOfLuns 2
  LunSizeInMB 300
  BlockSize 512

[AIO]
  AIO $curdir/aio0 aio0 512
  AIO $curdir/aio1 aio1 512

[OCF]
  OCF MalCache1 wt Malloc0 Malloc1
  OCF AIOCache1 pt Malloc0 aio0
  OCF AIOCache2 wb Malloc0 aio1
" > $curdir/modes.conf

fio_verify --filename=MalCache1:AIOCache1:AIOCache2 --spdk_conf=$curdir/modes.conf

trap - SIGINT SIGTERM EXIT
cleanup
