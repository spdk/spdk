#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#

apt-get install -y gcc g++ make libcunit1-dev libaio-dev libssl-dev libjson-c-dev libcmocka-dev uuid-dev libiscsi-dev
apt-get install -y libncurses5-dev libncursesw5-dev python3 python3-pip python3-dev
pip3 install ninja
pip3 install meson
pip3 install pyelftools
pip3 install ijson
pip3 install python-magic
pip3 install grpcio
pip3 install grpcio-tools
pip3 install pyyaml
# Additional dependencies for SPDK CLI
apt-get install -y python3-configshell-fb python3-pexpect

# Additional dependencies for DPDK
apt-get install -y nasm libnuma-dev
# Additional dependencies for ISA-L used in compression
apt-get install -y autoconf automake libtool help2man
# Additional dependencies for USDT
apt-get install -y systemtap-sdt-dev
if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
	# Tools for developers
	apt-get install -y git astyle lcov clang sg3-utils pciutils shellcheck \
		abigail-tools bash-completion ruby-dev pycodestyle
	# Additional dependencies for nvmf performance test script
	apt-get install -y python3-paramiko
fi
if [[ $INSTALL_PMEM == "true" ]]; then
	# Additional dependencies for building pmem based backends
	apt-get install -y libpmem-dev libpmemblk-dev libpmemobj-dev
fi
if [[ $INSTALL_FUSE == "true" ]]; then
	# Additional dependencies for FUSE and NVMe-CUSE
	apt-get install -y libfuse3-dev
fi
if [[ $INSTALL_RBD == "true" ]]; then
	# Additional dependencies for RBD bdev in NVMe over Fabrics
	apt-get install -y librados-dev librbd-dev
fi
if [[ $INSTALL_RDMA == "true" ]]; then
	# Additional dependencies for RDMA transport in NVMe over Fabrics
	apt-get install -y libibverbs-dev librdmacm-dev
fi
if [[ $INSTALL_DOCS == "true" ]]; then
	# Additional dependencies for building docs
	apt-get install -y doxygen mscgen graphviz
fi
# Additional dependencies for Avahi
if [[ $INSTALL_AVAHI == "true" ]]; then
	# Additional dependencies for Avahi
	apt-get install -y libavahi-client-dev
fi
