#!/usr/bin/env bash
set -x

modprobe nvme-tcp
modprobe nbd
export spdk=/root/spdk
export cpu_mask=$1
export HUGEMEM=$2
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$spdk/build/lib:$spdk/dpdk/build/lib"
SPDK_SOCK=$3
if [ -z "$SPDK_SOCK" ] ; then
  SPDK_SOCK=/var/tmp/spdk.sock
fi
export SPDK_SOCK
$spdk/scripts/setup.sh
$spdk/build/bin/spdk_tgt -m "$cpu_mask" -r "$SPDK_SOCK"