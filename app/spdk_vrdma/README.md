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
===========================
How to use SPDK test on ARM 
===========================
Basic BF2 version
==================
DOCA_v1.1_BlueField_OS_CentOS_7.6-5.4.0-1013.11.gc93744f-bluefield-5.4-0.6.5.0-3.7.0.11797-1-aarch64

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
# Since we hardcode use virtio-net before FW ready, we need configure net in NVCONFIG
# mlxconfig -d /dev/mst/mt41686_pciconf0 -y s VIRTIO_NET_EMULATION_ENABLE=1 \
#                      PCI_SWITCH_EMULATION_ENABLE=0 \
#                      NVME_EMULATION_ENABLE=0 \
#                      VIRTIO_NET_EMULATION_NUM_PF=1 \
#                      VIRTIO_NET_EMULATION_NUM_VF=4 \
#                      VIRTIO_NET_EMULATION_NUM_MSIX=16 \
#                      NUM_VF_MSIX=64  NUM_OF_VFS=16 \
#                      PF_BAR2_ENABLE=0 PER_PF_NUM_SF=1 PF_TOTAL_SF=150 PF_SF_BAR_SIZE=10 \
#                      SRIOV_EN=1
# Note: need FW reset to let NVCONFIG work.

export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/snap/lib #will be deleted this step in release version
echo 4096 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
# systemctl stop virtio-net-controller  #will be deleted this step after FW ready

<spdk_vrdma_view>./app/spdk_vrdma/spdk_vrdma

#Note:Must run spdk on ARM before dpdk on host to make sure device reset successfully.

=================================
How to use DPDK test on host 
=================================
How to get code
==============================
git clone git@github.com:LiZhang-2020/dpdk.org.git  dpdk_vrdma_view
cd dpdk_vrdma_view
<dpdk_vrdma_view>git checkout lizh-dpdk-vrdma-test-22-11-rc1

How to build
==============================
meson build -Dexamples=vdpa && ninja -C build

How to run testpmd
==============================
#1. Set hugepage for testpmd
echo 4096 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages

#2. Find vrdma device pci, such as 5e:00.3
lspci |grep Tencent
5e:00.3 Ethernet controller: Tencent Technology (Shenzhen) Company Limited Device 4ace

#3. Bond vrmda device to dpdk manage it, such as 0000:5e:00.3
/swgwork/lizh/dpdk_vrdma_test_1104/usertools/dpdk-devbind.py --bind=vfio-pci 0000:5e:00.3

#4. Start testpmd with vrdma device, such as 5e:00.3
sudo ./build/app/dpdk-testpmd --log-level=.,8 -a 5e:00.3 --iova-mode=pa  -- -i --rxq=1 --txq=1 -a --enable-rx-cksum --no-flush-rx

How to run testpmd command
==============================
create vrdma adminq 5e:00.3 0         #Will get admin-queue by vrdma device 5e:00.3
create vrdma adminq msg 0 106         #Will create PD index 0 for vrdma device on ARM.
create vrdma qp 0 4                   #Will 4 qps for test traffic
#dump vrdma adminq 0           #Will dump admin-queue message
#del vrdma adminq 0

How to run RPC command on ARM
==============================
#Configure qp connect information by RPC command.
#<spdk_vrdma_view> snap-rdma/rpc/snap_rpc.py controller_vrdma_configue -d <device_id> -e mlx5_0 -v <vrdma_qpn > -b <backend_rqpn> -c <dest_mac> -u <subnet_prefix> -i <intf_id>
One example:
snap-rdma/rpc/snap_rpc.py controller_vrdma_configue -d 0 -e mlx5_0  -v 2 -b 2 -c 0x66778899aa -u  0x1234 -i 0x5678

How to run testpmd command for qp ready
==============================
# modify vrdma qp <dev_id> <qp_idx> <qp_state> <dest_qpn> <sip> <dip>
One example:
modify vrdma qp 0 2 1 2 0x1234 0x5678  #Set qp to init
modify vrdma qp 0 2 2 2 0x1234 0x5678  #Set qp to RTR
modify vrdma qp 0 2 3 2 0x1234 0x5678  #Set qp to RTS