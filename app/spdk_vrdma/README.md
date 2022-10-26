/*-
 *   BSD LICENSE
 *
 *   Copyright © 2022 NVIDIA CORPORATION & AFFILIATES. ALL RIGHTS RESERVED.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

How to get code
==================
git clone git@github.com:LiZhang-2020/spdk.git spdk_vrdma_view
cd spdk_vrdma_view
<spdk_vrdma_view>git checkout lizh-spdk-vrdma-19-10-1
<spdk_vrdma_view>git submodule update --init
#Note: need snap-rdma username and token to get snap-rdma submodule

How to build
==================
#username must be root
<spdk_vrdma_view>./scripts/pkgdep.sh
<spdk_vrdma_view>./configure
<spdk_vrdma_view>make

How to run
==================
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/snap/lib #will be deleted this step in release version
echo 4096 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
systemctl stop virtio-net-controller  #will be deleted this step after FW ready

<spdk_vrdma_view>./app/spdk_vrdma/spdk_vrdma

Basic BF2 version
==================
DOCA_v1.1_BlueField_OS_CentOS_7.6-5.4.0-1013.11.gc93744f-bluefield-5.4-0.6.5.0-3.7.0.11797-1-aarch64
