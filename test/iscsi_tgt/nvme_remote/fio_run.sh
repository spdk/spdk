#!/usr/bin/env bash

set -e

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)

fio_py="python $rootdir/scripts/fio.py"

echo "Running FIO"
$fio_py 4096 1 randrw 5 verify
