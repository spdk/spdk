BLOCKDEV_MODULES += $(SPDK_ROOT_DIR)/lib/bdev/malloc/libspdk_bdev_malloc.a

BLOCKDEV_MODULES += $(SPDK_ROOT_DIR)/lib/bdev/nvme/libspdk_bdev_nvme.a \
		    $(SPDK_ROOT_DIR)/lib/nvme/libspdk_nvme.a

COPY_MODULES += $(SPDK_ROOT_DIR)/lib/copy/ioat/libspdk_copy_ioat.a \
		$(SPDK_ROOT_DIR)/lib/ioat/libspdk_ioat.a

BLOCKDEV_MODULES_LINKER_ARGS = -Wl,--whole-archive \
			       $(BLOCKDEV_MODULES) \
			       -Wl,--no-whole-archive \
			       $(BLOCKDEV_MODULES_DEPS)

COPY_MODULES_LINKER_ARGS = -Wl,--whole-archive \
			   $(COPY_MODULES) \
			   -Wl,--no-whole-archive \
			   $(COPY_MODULES_DEPS)
