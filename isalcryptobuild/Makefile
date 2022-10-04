#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

.PHONY: all clean install

ifneq ($(Q),)
REDIRECT=> /dev/null
endif

# Force-disable scan-build
SUB_CC = $(patsubst %ccc-analyzer,$(DEFAULT_CC),$(CC))

$(ISAL_CRYPTO_DIR)/isa-l-crypto:
	@ln -s $(ISAL_CRYPTO_DIR)/include $(ISAL_CRYPTO_DIR)/isa-l-crypto

all: $(ISAL_CRYPTO_DIR)/isa-l-crypto
	$(Q)$(MAKE) -C $(SPDK_ROOT_DIR)/isa-l-crypto all CC="$(SUB_CC)" $(REDIRECT)

install: all

uninstall:
	@:

clean:
	$(Q)$(MAKE) -C $(SPDK_ROOT_DIR)/isa-l-crypto clean $(REDIRECT)
	$(Q)rm -rf $(ISAL_CRYPTO_DIR)/isa-l-crypto
