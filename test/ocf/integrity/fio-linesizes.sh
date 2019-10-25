#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin

source $rootdir/test/common/autotest_common.sh

function fio_verify(){
	fio_bdev $curdir/test.fio --aux-path=/tmp/ --ioengine=spdk_bdev $@
}

function cleanup(){
	rm -f $curdir/linesizes.conf
}

trap "cleanup; exit 1" SIGINT SIGTERM EXIT

nvme_cfg=$($rootdir/scripts/gen_nvme.sh)

config="
$nvme_cfg

[Split]
  Split Nvme0n1 8 101

[OCF]
  OCF WT_Nvme8k  pt  8192 Nvme0n1p0 Nvme0n1p1
  OCF WT_Nvme16k wt 16384 Nvme0n1p2 Nvme0n1p3
  OCF WT_Nvme32k wb 32768 Nvme0n1p4 Nvme0n1p5
  OCF WT_Nvme64k wb 65536 Nvme0n1p6 Nvme0n1p7
"
echo "$config" > $curdir/linesizes.conf

fio_verify --filename=PT_Nvme:WT_Nvme:WB_Nvme0:WB_Nvme1 --spdk_conf=$curdir/linesizes.conf

trap - SIGINT SIGTERM EXIT
cleanup
