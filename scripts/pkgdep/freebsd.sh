#!/usr/bin/env bash

# Minimal install
pkg install -y gmake cunit openssl git bash misc/e2fsprogs-libuuid python \
	ncurses ninja meson
pkg install -g -y "py*-pyelftools-*" "py*-pandas"
# Additional dependencies for ISA-L used in compression
pkg install -y autoconf automake libtool help2man
if [[ $INSTALL_DEV_TOOLS == "true" ]]; then
	# Tools for developers
	pkg install -y devel/astyle bash \
		misc/e2fsprogs-libuuid sysutils/sg3_utils nasm \
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
