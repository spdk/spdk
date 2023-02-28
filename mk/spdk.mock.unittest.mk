#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2015 Intel Corporation.
#  All rights reserved.
#

SPDK_MOCK_SYSCALLS += \
	calloc \
	pthread_mutexattr_init \
	pthread_mutex_init \
	recvmsg \
	sendmsg \
	writev

define add_wrap_with_prefix
$(2:%=-Wl,--wrap,$(1)%)
endef

ifeq ($(OS),Windows)
# Windows needs a thin layer above the system calls to provide POSIX
# functionality. For GCC, use the prefix wpdk_ to ensure that the layer
# is called. For other compilers, --wrap is not supported so the layer
# implements an alternative mechanism to enable mocking.
ifeq ($(CC_TYPE),gcc)
LDFLAGS += $(call add_wrap_with_prefix,wpdk_,$(SPDK_MOCK_SYSCALLS))
endif
else
LDFLAGS += $(call add_wrap_with_prefix,,$(SPDK_MOCK_SYSCALLS))
endif
