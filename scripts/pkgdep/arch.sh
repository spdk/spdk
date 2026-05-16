#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#
# Install main dependencies
pacman -Sy --needed --noconfirm gcc make cunit libaio openssl \
	libutil-linux libiscsi python ncurses json-c cmocka ninja meson fuse3
# Additional dependencies for SPDK CLI
pacman -Sy --needed --noconfirm python-pexpect python-pip libffi

pkgdep_setup_python_venv "$rootdir"

# Additional dependencies for DPDK
pacman -Sy --needed --noconfirm numactl nasm
# Additional dependencies for ISA-L used in compression
pacman -Sy --needed --noconfirm autoconf automake libtool help2man
if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
	# Tools for developers
	pacman -Sy --needed --noconfirm git astyle autopep8 \
		clang sg3_utils pciutils shellcheck bash-completion
	#fakeroot needed to install via makepkg
	pacman -Sy --needed --noconfirm fakeroot
	su - $SUDO_USER -c "pushd /tmp;
		git clone https://aur.archlinux.org/perl-perlio-gzip.git;
		cd perl-perlio-gzip;
		yes y | makepkg -si --needed;
		cd ..; rm -rf perl-perlio-gzip
		popd"
	# sed is to modify sources section in PKGBUILD
	# By default it uses git:// which will fail behind proxy, so
	# redirect it to http:// source instead
	su - $SUDO_USER -c "pushd /tmp;
		git clone https://aur.archlinux.org/lcov-git.git;
		cd lcov-git;
		sed -i 's/git:/git+http:/' PKGBUILD;
		makepkg -si --needed --noconfirm;
		cd .. && rm -rf lcov-git;
		popd"
fi
if [[ $INSTALL_RBD == "true" ]]; then
	echo "Arch Linux does not have librados-devel and librbd-devel in mainline repositories."
	echo "You can install them manually"
fi
if [[ $INSTALL_RDMA == "true" ]]; then
	# Additional dependencies for RDMA transport in NVMe over Fabrics
	if [[ -n "$http_proxy" ]]; then
		gpg_options=" --keyserver hkp://pgp.mit.edu:11371 --keyserver-options \"http-proxy=$http_proxy\""
	fi
	su - $SUDO_USER -c "gpg $gpg_options --recv-keys 29F0D86B9C1019B1"
	su - $SUDO_USER -c "pushd /tmp;
		git clone https://aur.archlinux.org/rdma-core.git;
		cd rdma-core;
		makepkg -si --needed --noconfirm;
		cd .. && rm -rf rdma-core;
		popd"
fi
if [[ $INSTALL_DOCS == "true" ]]; then
	# Additional dependencies for building docs
	pacman -Sy --needed --noconfirm doxygen graphviz
	pacman -S --noconfirm --needed gd ttf-font
	su - $SUDO_USER -c "pushd /tmp;
		git clone https://aur.archlinux.org/mscgen.git;
		cd mscgen;
		makepkg -si --needed --noconfirm;
		cd .. && rm -rf mscgen;
		popd"
fi
