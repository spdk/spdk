#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin

source $rootdir/test/common/autotest_common.sh

function fio_verify(){
	LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio $curdir/test.fio --aux-path=/tmp/ --ioengine=spdk_bdev $@
}

function cleanup(){
	rm -f $curdir/aio_*
	rm -f $curdir/modes.conf
}

truncate -s 101M $curdir/aio_core_m0
truncate -s 101M $curdir/aio_core_m1

truncate -s 101M $curdir/aio_cache_pt
truncate -s 101M $curdir/aio_core_pt
truncate -s 101M $curdir/aio_cache_wb
truncate -s 101M $curdir/aio_core_wb

trap "cleanup; exit 1" SIGINT SIGTERM EXIT

echo "
[Malloc]
  NumberOfLuns 1
  LunSizeInMB 300
  BlockSize 512

[AIO]
  AIO $curdir/aio_core_m0  aio_core_m0  512
  AIO $curdir/aio_core_m1  aio_core_m1  512

  AIO $curdir/aio_cache_pt aio_cache_pt 512
  AIO $curdir/aio_core_pt  aio_core_pt  512
  AIO $curdir/aio_cache_wb aio_cache_wb 512
  AIO $curdir/aio_core_wb  aio_core_wb  512

[OCF]
  OCF WT_MCache-0 wt Malloc0      aio_core_m0
  OCF WT_MCache-1 wt Malloc0      aio_core_m1

  OCF PT_AIOCache pt aio_cache_pt aio_core_pt
  OCF WB_AIOCache wb aio_cache_wb aio_core_wb
" > $curdir/modes.conf

fio_verify --filename=WT_MCache-0:WT_MCache-1:PT_AIOCache:WB_AIOCache --spdk_conf=$curdir/modes.conf

trap - SIGINT SIGTERM EXIT
cleanup
