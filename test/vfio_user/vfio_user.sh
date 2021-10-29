#!/usr/bin/env bash
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vhost/common.sh
source $rootdir/test/vfio_user/common.sh

echo "Running SPDK vfio-user fio autotest..."

vhosttestinit

run_test "vfio_user_fio" $WORKDIR/vfio_user_fio/vfio_user_fio.sh

vhosttestfini
