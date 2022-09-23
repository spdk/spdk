: ${MALLOC_BDEV_SIZE=256}
: ${MALLOC_BLOCK_SIZE=512}

source "$rootdir/test/vhost/common.sh"

# Verify vfio-user support of qemu.
VFIO_QEMU_BIN=${VFIO_QEMU_BIN:-/usr/local/qemu/vfio-user-irqmask2/bin/qemu-system-x86_64}

if [[ ! -e $VFIO_QEMU_BIN ]]; then
	error "$VFIO_QEMU_BIN QEMU not found, cannot run the vfio-user tests"
	return 1
fi

QEMU_BIN=$VFIO_QEMU_BIN
