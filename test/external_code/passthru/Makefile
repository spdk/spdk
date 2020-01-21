#
#  BSD LICENSE
#
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions
#  are met:
#
#    * Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#    * Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in
#      the documentation and/or other materials provided with the
#      distribution.
#    * Neither the name of Intel Corporation nor the names of its
#      contributors may be used to endorse or promote products derived
#      from this software without specific prior written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
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
