#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/vfio_user/common.sh

echo "Running SPDK vfio-user fio autotest..."

vhosttestinit

run_test "vfio_user_nvme_fio" $WORKDIR/nvme/vfio_user_fio.sh
run_test "vfio_user_nvme_restart_vm" $WORKDIR/nvme/vfio_user_restart_vm.sh
run_test "vfio_user_virtio_blk_restart_vm" $WORKDIR/virtio/fio_restart_vm.sh virtio_blk
run_test "vfio_user_virtio_scsi_restart_vm" $WORKDIR/virtio/fio_restart_vm.sh virtio_scsi
run_test "vfio_user_virtio_bdevperf" $WORKDIR/virtio/initiator_bdevperf.sh

vhosttestfini
