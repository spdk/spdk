#!/usr/bin/env bash

VERSION_ID_NUM=$(sed 's/\.//g' <<< $VERSION_ID)
# Includes Ubuntu, Debian
# Minimal install
apt-get install -y gcc g++ make libcunit1-dev libaio-dev libssl-dev libjson-c-dev libcmocka-dev \
	uuid-dev libiscsi-dev python libncurses5-dev libncursesw5-dev python3-pip
pip3 install ninja
if ! pip3 install meson; then
	# After recent updates pip3 on ubuntu1604 provides meson version which requires python >= 3.6.
	# Unfortunately, the latest available version of python3 there is 3.5.2. In case pip3 fails to
	# install meson fallback to packaged version of it ubuntu1604's repos may provide.
	apt-get install -y meson
fi
pip3 install pyelftools
pip3 install ijson
pip3 install python-magic
# Additional dependencies for SPDK CLI - not available on older Ubuntus
apt-get install -y python3-configshell-fb python3-pexpect || echo \
	"Note: Some SPDK CLI dependencies could not be installed."

# Additional dependencies for DPDK
if [[ $NAME == "Ubuntu" ]] && [[ $VERSION_ID_NUM -lt 1900 ]]; then
	echo "Ubuntu $VERSION_ID needs NASM version 2.14 for DPDK but is not in the mainline repository."
	echo "You can install it manually"
else
	apt-get install -y nasm
fi
apt-get install -y libnuma-dev
# Additional dependencies for ISA-L used in compression
apt-get install -y autoconf automake libtool help2man
# Additional dependencies for USDT
apt-get install -y systemtap-sdt-dev
if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
	# Tools for developers
	apt-get install -y git astyle pep8 lcov clang sg3-utils pciutils shellcheck \
		abigail-tools bash-completion ruby-dev
	# Additional python style checker not available on ubuntu 16.04 or earlier.
	apt-get install -y pycodestyle || true
	# Additional dependencies for nvmf performance test script
	apt-get install -y python3-paramiko
fi
if [[ $INSTALL_PMEM == "true" ]]; then
	# Additional dependencies for building pmem based backends
	if [[ $NAME == "Ubuntu" ]] && [[ $VERSION_ID_NUM -gt 1800 ]]; then
		apt-get install -y libpmem-dev
		apt-get install -y libpmemblk-dev
		apt-get install -y libpmemobj-dev
	fi
fi
if [[ $INSTALL_FUSE == "true" ]]; then
	# Additional dependencies for FUSE and NVMe-CUSE
	if [[ $NAME == "Ubuntu" ]] && ((VERSION_ID_NUM > 1400 && VERSION_ID_NUM < 1900)); then
		echo "Ubuntu $VERSION_ID does not have libfuse3-dev in mainline repository."
		echo "You can install it manually"
	else
		apt-get install -y libfuse3-dev
	fi
fi
if [[ $INSTALL_RDMA == "true" ]]; then
	# Additional dependencies for RDMA transport in NVMe over Fabrics
	apt-get install -y libibverbs-dev librdmacm-dev
fi
if [[ $INSTALL_DOCS == "true" ]]; then
	# Additional dependencies for building docs
	apt-get install -y doxygen mscgen graphviz
fi
