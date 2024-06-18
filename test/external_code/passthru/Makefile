#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation.
#  All rights reserved.
#

src=vbdev_passthru_rpc.c vbdev_passthru.c
shared:
	$(CC) $(COMMON_CFLAGS) -c -fPIC ./vbdev_passthru_rpc.c -o ./vbdev_passthru_rpc.o
	$(CC) $(COMMON_CFLAGS) -c -fPIC ./vbdev_passthru.c -o ./vbdev_passthru.o
	$(CC) $(COMMON_CFLAGS) -shared ./vbdev_passthru_rpc.o ./vbdev_passthru.o -o ./libpassthru_external.so

static:
	$(CC) $(COMMON_CFLAGS) -c ./vbdev_passthru_rpc.c -o ./vbdev_passthru_rpc.o
	$(CC) $(COMMON_CFLAGS) -c ./vbdev_passthru.c -o ./vbdev_passthru.o
	$(AR) rcs ./libpassthru_external.a ./vbdev_passthru_rpc.o ./vbdev_passthru.o
