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
  Split Nvme0n1 8 105

[OCF]
  OCF WT_Nvme  wt Nvme0n1p2 Nvme0n1p3
"
echo "$config" > $curdir/modes.conf

fio_verify --filename=WT_Nvme --spdk_conf=$curdir/modes.conf

trap - SIGINT SIGTERM EXIT
cleanup
