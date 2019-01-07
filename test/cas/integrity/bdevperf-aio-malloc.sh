#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
source $curdir/common.sh

prefix=$(basename "$BASH_SOURCE")"$RANDOM"

# INIT
truncate -s 10G /tmp/${prefix}.bin

cat > /tmp/${prefix}.conf << EOL
[AIO]
  AIO /tmp/${prefix}.bin aio_file 512

[Malloc]
  NumberOfLuns 1
  LunSizeInMB 300
  BlockSize 512

[CAS]
  CAS Cache-A wt Malloc0 aio_file
EOL

# MAIN
bdevperf_verify -c /tmp/${prefix}.conf -q 128 -o 4096 -t 5
status=$?

# CLEANUP
rm -f /tmp/${prefix}.bin
rm -f /tmp/${prefix}.conf

exit $status
