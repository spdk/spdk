#!/usr/bin/env bash

curdir=$(dirname $(readlink -f "$BASH_SOURCE"))
rootdir=$(readlink -f $curdir/../../..)
source $curdir/common.sh

# INIT
truncate -s 10G /tmp/aio-file1.bin

cat > /tmp/aio-malloc.conf << EOL
[AIO]
  AIO /tmp/aio-file1.bin aio_file 512

[Malloc]
  NumberOfLuns 1
  LunSizeInMB 300
  BlockSize 512

[Cache]
  CAS Cache-A wt Malloc0 aio_file
EOL

# MAIN
verify -c /tmp/aio-malloc.conf -q 128 -o 4096 -t 5
status=$?

# CLEANUP
rm -f /tmp/aio-file1.bin
rm -f /tmp/aio-malloc.conf

exit $status
