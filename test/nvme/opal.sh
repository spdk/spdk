#!/usr/bin/env bash

set -xe

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/scripts/common.sh
source $rootdir/test/common/autotest_common.sh

$rootdir/scripts/setup.sh

timing_enter opal
for bdf in $(iter_pci_class_code 01 08 02); do
         printf '8\n%s\n1\n\n2\ntest\n\n\n3\ntest\n\n\n0\n\n9\n' ${bdf} | $rootdir/examples/nvme/nvme_manage/nvme_manage
done
timing_exit opal
