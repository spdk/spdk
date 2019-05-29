#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
plugindir=$rootdir/examples/bdev/fio_plugin

source $rootdir/test/common/autotest_common.sh

function fio_verify(){
	LD_PRELOAD=$plugindir/fio_plugin /usr/src/fio/fio $curdir/test.fio --aux-path=/tmp/ --ioengine=spdk_bdev $@
}

function cleanup(){
	rm -f $curdir/modes.conf
}

trap "cleanup; exit 1" SIGINT SIGTERM EXIT

nvme_cfg=$($rootdir/scripts/gen_nvme.sh)

config="
$nvme_cfg

[Split]
  Split Nvme0n1 16 101

[OCF]
  OCF PT_Nvme  pt Nvme0n1p0 Nvme0n1p2
  OCF WT_Nvme  wt Nvme0n1p4 Nvme0n1p6
  OCF WB_Nvme0 wb Nvme0n1p8 Nvme0n1p10
  OCF WB_Nvme1 wb Nvme0n1p12 Nvme0n1p14
"
echo "$config" > $curdir/modes.conf

fio_verify --filename=PT_Nvme:WT_Nvme:WB_Nvme0:WB_Nvme1 --spdk_conf=$curdir/modes.conf

trap - SIGINT SIGTERM EXIT
cleanup
