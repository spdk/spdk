#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation
#  All rights reserved.
#

: ${MALLOC_BDEV_SIZE=256}
: ${MALLOC_BLOCK_SIZE=512}

source "$rootdir/test/vhost/common.sh"

# Verify vfio-user support of qemu.
if [[ ! -e $VFIO_QEMU_BIN ]]; then
	error "$VFIO_QEMU_BIN QEMU not found, cannot run the vfio-user tests"
	return 1
fi

QEMU_BIN=$VFIO_QEMU_BIN
