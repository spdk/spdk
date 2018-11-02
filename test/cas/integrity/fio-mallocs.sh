#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
source $curdir/common.sh

prefix=$(basename "$BASH_SOURCE")"$RANDOM"

# INIT

cat > /tmp/${prefix}.conf << EOL
[Malloc]
  NumberOfLuns 4
  LunSizeInMB 300
  BlockSize 512

[CAS]
  CAS MalCache1 wt Malloc0 Malloc1
  CAS MalCache2 pt Malloc2 Malloc3
EOL

# RUN
fio_verify --filename=MalCache1:MalCache2 --spdk_conf=/tmp/${prefix}.conf
status=$?

# CLEANUP
rm -f /tmp/${prefix}.conf

exit $status
