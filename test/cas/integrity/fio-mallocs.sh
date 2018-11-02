#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
source $curdir/common.sh

prefix=$(basename "$BASH_SOURCE")"$RANDOM"

# INIT

cat > /tmp/${prefix}.conf << EOL
[Malloc]
  NumberOfLuns 2
  LunSizeInMB 300
  BlockSize 512

[CAS]
  CAS MalCache wt Malloc0 Malloc1
EOL

# MAIN
fio_verify --filename=MalCache --spdk_conf=/tmp/${prefix}.conf
status=$?

# CLEANUP
rm -f /tmp/${prefix}.conf

exit $status
