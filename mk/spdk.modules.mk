BLOCKDEV_MODULES += $(SPDK_ROOT_DIR)/build/lib/libspdk_bdev_malloc.a

BLOCKDEV_MODULES += $(SPDK_ROOT_DIR)/build/lib/libspdk_bdev_nvme.a \
		    $(SPDK_ROOT_DIR)/build/lib/libspdk_nvme.a

ifeq ($(OS),Linux)
BLOCKDEV_MODULES += $(SPDK_ROOT_DIR)/build/lib/libspdk_bdev_aio.a
BLOCKDEV_MODULES_DEPS += -laio
endif

ifeq ($(CONFIG_RBD),y)
BLOCKDEV_MODULES += $(SPDK_ROOT_DIR)/build/lib/libspdk_bdev_rbd.a
BLOCKDEV_MODULES_DEPS += -lrados -lrbd
endif

COPY_MODULES += $(SPDK_ROOT_DIR)/build/lib/libspdk_copy_ioat.a \
		$(SPDK_ROOT_DIR)/build/lib/libspdk_ioat.a

BLOCKDEV_MODULES_LINKER_ARGS = -Wl,--whole-archive \
			       $(BLOCKDEV_MODULES) \
			       -Wl,--no-whole-archive \
			       $(BLOCKDEV_MODULES_DEPS)

COPY_MODULES_LINKER_ARGS = -Wl,--whole-archive \
			   $(COPY_MODULES) \
			   -Wl,--no-whole-archive \
			   $(COPY_MODULES_DEPS)
