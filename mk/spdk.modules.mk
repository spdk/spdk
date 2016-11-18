BLOCKDEV_MODULES_LIST = bdev_malloc bdev_nvme nvme

ifeq ($(CONFIG_RDMA),y)
BLOCKDEV_MODULES_DEPS += -libverbs -lrdmacm
endif

ifeq ($(OS),Linux)
BLOCKDEV_MODULES_LIST += bdev_aio
BLOCKDEV_MODULES_DEPS += -laio
endif

ifeq ($(CONFIG_RBD),y)
BLOCKDEV_MODULES_LIST += bdev_rbd
BLOCKDEV_MODULES_DEPS += -lrados -lrbd
endif

COPY_MODULES_LIST = copy_ioat ioat

BLOCKDEV_MODULES_LINKER_ARGS = -Wl,--whole-archive \
			       $(BLOCKDEV_MODULES_LIST:%=-lspdk_%) \
			       -Wl,--no-whole-archive \
			       $(BLOCKDEV_MODULES_DEPS)

BLOCKDEV_MODULES_FILES = $(call spdk_lib_list_to_files,$(BLOCKDEV_MODULES_LIST))

COPY_MODULES_LINKER_ARGS = -Wl,--whole-archive \
			   $(COPY_MODULES_LIST:%=-lspdk_%) \
			   -Wl,--no-whole-archive \
			   $(COPY_MODULES_DEPS)

COPY_MODULES_FILES = $(call spdk_lib_list_to_files,$(COPY_MODULES_LIST))
