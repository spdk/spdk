#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2020 Intel Corporation
#  All rights reserved.
#
# Minimal install
pkg install -y gmake cunit openssl git bash python ncurses ninja meson
pkg install -g -y "py*-pyelftools-*" "py*-pandas"
# Additional dependencies for ISA-L used in compression
pkg install -y autoconf automake libtool help2man
if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
	# Tools for developers
	pkg install -y devel/astyle bash sysutils/sg3_utils nasm \
		bash-completion ruby devel/ruby-gems
	pkg install -g -y "py*-pycodestyle"
	# ruby and ruby-gems are not preinstalled on FreeBSD but are needed to
	# build mdl - make sure they are in place.
	pkg install -y ruby devel/ruby-gems
fi
if [[ $INSTALL_DOCS == "true" ]]; then
	# Additional dependencies for building docs
	pkg install -y doxygen mscgen graphviz
fi

if [[ $INSTALL_LIBURING == "true" ]]; then
	printf 'liburing is not supported on %s, disabling\n' \
		"$(freebsd-version)"
	INSTALL_LIBURING=false
fi

if [[ $INSTALL_RBD == "true" ]]; then
	# Additional dependencies for RBD bdev in NVMe over Fabrics
	pkg install -y ceph || pkg install -y ceph14
fi
