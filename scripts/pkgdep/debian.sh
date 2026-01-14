#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#  Copyright (c) 2022 Dell Inc, or its subsidiaries.
#

apt-get install -y gcc g++ make libcunit1-dev libaio-dev libssl-dev libjson-c-dev libcmocka-dev uuid-dev libiscsi-dev \
	libkeyutils-dev libncurses5-dev libncursesw5-dev python3 python3-pip python3-dev unzip libfuse3-dev patchelf \
	curl procps pkgconf python3-venv

# per PEP668 work inside virtual env
virtdir=${PIP_VIRTDIR:-/var/spdk/dependencies/pip}
if python3 -c 'import sys; exit(0 if sys.version_info >= (3,9) else 1)'; then
	python3 -m venv --upgrade-deps --system-site-packages "$virtdir"
else
	# --upgrade-deps was introduced only in Python 3.9.0 (October 5, 2020).
	python3 -m venv --system-site-packages "$virtdir"
	"$virtdir"/bin/pip install --upgrade pip setuptools
fi
pkgdep_toolpath pip "$virtdir/bin"
source "$virtdir/bin/activate"
python -m pip install pip-tools
pip-compile --extra dev --strip-extras -o "$rootdir/scripts/pkgdep/requirements.txt" "${rootdir}/python/pyproject.toml"
pip3 install -r "$rootdir/scripts/pkgdep/requirements.txt"

# Fixes issue: #3721
pkgdep_toolpath meson "${virtdir}/bin"

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
