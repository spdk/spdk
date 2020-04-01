#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "${BASH_SOURCE[0]}"))
rootdir=$(readlink -f $curdir/../../..)

source $rootdir/test/ocf/common.sh

function fio_verify(){
	fio_bdev $curdir/test.fio --aux-path=/tmp/ --ioengine=spdk_bdev "$@"
}

function cleanup(){
	rm -f $curdir/modes.conf
}

# Clear nvme device which we will use in test
clear_nvme

trap "cleanup; exit 1" SIGINT SIGTERM EXIT

nvme_cfg=$($rootdir/scripts/gen_nvme.sh)

config="
$(nvme_cfg)

[Split]
  Split Nvme0n1 8 101

[OCF]
  OCF PT_Nvme  pt Nvme0n1p0 Nvme0n1p1
  OCF WT_Nvme  wt Nvme0n1p2 Nvme0n1p3
  OCF WB_Nvme0 wb Nvme0n1p4 Nvme0n1p5
  OCF WB_Nvme1 wb Nvme0n1p6 Nvme0n1p7
"
echo "$config" > $curdir/modes.conf

fio_verify --filename=PT_Nvme:WT_Nvme:WB_Nvme0:WB_Nvme1 --spdk_conf=$curdir/modes.conf

trap - SIGINT SIGTERM EXIT
cleanup
