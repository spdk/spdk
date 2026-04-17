#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#

apt-get() {
	"$(type -P apt-get)" -o Acquire::Retries=3 "$@"
}

apt-get install -y gcc g++ make libcunit1-dev libaio-dev libssl-dev libjson-c-dev libcmocka-dev uuid-dev libiscsi-dev \
	libkeyutils-dev libncurses5-dev libncursesw5-dev python3 python3-pip python3-dev unzip libfuse3-dev patchelf \
	curl procps pkgconf python3-venv

# use python3.9 for Ubuntu <= 20.04 that ships with python3.8
if [[ $ID == "ubuntu" && ${VERSION_ID:0:2} -le "20" ]]; then
	apt-get install -y python3.9 python3.9-venv python3-pip python3.9-dev
	update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.9 1
	update-alternatives --set python3 /usr/bin/python3.9
fi

pkgdep_setup_python_venv "$rootdir"

# Additional dependencies for SPDK CLI
apt-get install -y python3-configshell-fb python3-pexpect
# Additional dependencies for code generation
apt-get install -y python3-tabulate
# Additional dependencies for DPDK
apt-get install -y nasm libnuma-dev
# Additional dependencies for ISA-L used in compression
apt-get install -y autoconf automake libtool help2man
# Additional dependencies for USDT
apt-get install -y systemtap-sdt-dev
if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
	# Tools for developers
	apt-get install -y git cmake lcov clang sg3-utils pciutils shellcheck \
		abigail-tools bash-completion ruby-dev pycodestyle bundler rake
	# Additional dependencies for nvmf performance test script
	apt-get install -y python3-paramiko
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
if [[ $INSTALL_IDXD == "true" ]]; then
	# accel-config-devel is required for kernel IDXD implementation used in DSA accel module
	if [[ $ID == "ubuntu" && ${VERSION_ID:0:2} -ge "23" ]]; then
		apt-get install -y libaccel-config-dev
	else
		echo "libaccel-config is only present on Ubuntu 23.04 or higher."
	fi
fi
if [[ $INSTALL_LZ4 == "true" ]]; then
	apt-get install -y liblz4-dev
fi
