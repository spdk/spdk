#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  All rights reserved.
#

set -e

SPDK_DIR=$(readlink -f "$(dirname "$0")")/..
export SPDK_DIR
ROOT_DIR=$(readlink -f "$SPDK_DIR/..")
export ROOT_DIR
export CROSS_COMPILE_DIR=$ROOT_DIR/cross_compiling
export DPDK_DIR=$SPDK_DIR/dpdk

: "${CROSS_PREFIX:=riscv64-linux-gnu}"
: "${TARGET_ARCH:=rv64gc}"
export SYSROOT=$CROSS_COMPILE_DIR/sysroot

# Host toolchain check
function check_toolchain() {
	if ! command -v "${CROSS_PREFIX}-gcc" > /dev/null 2>&1; then
		echo "error: ${CROSS_PREFIX}-gcc not found in PATH."
		echo "       Install a riscv64 GNU/Linux cross toolchain from your distro,"
		echo "       or build one from riscv-collab/riscv-gnu-toolchain, and ensure"
		echo "       it is on PATH. Override the toolchain prefix via CROSS_PREFIX if needed."
		exit 1
	fi
	echo -e "Using toolchain: $(command -v ${CROSS_PREFIX}-gcc)"
}

# NUMA
function cross_compile_numa() {
	cd $CROSS_COMPILE_DIR

	if [ ! -d "$CROSS_COMPILE_DIR/numactl" ]; then
		echo -e "Downloading NUMA library..."
		git clone --depth=1 --branch v2.0.16 https://github.com/numactl/numactl.git
	else
		echo -e "NUMA library already downloaded"
	fi
	cd numactl/

	if [ ! -f "$SYSROOT/usr/lib/libnuma.so" ]; then
		echo -e "Building NUMA library..."
		./autogen.sh
		./configure --host=${CROSS_PREFIX} CC=${CROSS_PREFIX}-gcc --prefix=$SYSROOT/usr --libdir=$SYSROOT/usr/lib
		make clean
		make -j install
	else
		echo -e "NUMA library already built"
	fi
}

# util-linux UUID
function cross_compile_uuid() {
	cd $CROSS_COMPILE_DIR

	if [ ! -d "$CROSS_COMPILE_DIR/util-linux" ]; then
		echo -e "Downloading util-linux UUID library..."
		git clone --depth=1 --branch v2.40.2 https://github.com/util-linux/util-linux.git
	else
		echo -e "util-linux UUID library already downloaded"
	fi

	if [ ! -f "$SYSROOT/usr/lib/libuuid.so" ]; then
		cd util-linux/

		echo -e "Building util-linux UUID library..."

		./autogen.sh
		CC=${CROSS_PREFIX}-gcc CXX=${CROSS_PREFIX}-g++ LD=${CROSS_PREFIX}-ld \
			./configure --host=${CROSS_PREFIX} \
			--prefix=$SYSROOT/usr --libdir=$SYSROOT/usr/lib \
			--disable-all-programs --enable-libuuid \
			--without-tinfo --without-ncurses --without-ncursesw \
			--disable-mount --disable-libmount --disable-pylibmount \
			--disable-libblkid --disable-fdisks --disable-libfdisk
		make clean
		make -j
		make -j install
	else
		echo -e "util-linux UUID library already built"
	fi
}

# OpenSSL Crypto and SSL
function cross_compile_crypto_ssl() {
	cd $CROSS_COMPILE_DIR

	if [ ! -d "$CROSS_COMPILE_DIR/openssl" ]; then
		echo -e "Downloading OpenSSL Crypto and SSL libraries..."
		git clone --depth=1 --branch openssl-3.3.2 https://github.com/openssl/openssl.git
	else
		echo -e "OpenSSL Crypto and SSL libraries already downloaded"
	fi

	if [ ! -f "$SYSROOT/usr/lib/libssl.so" ]; then
		cd openssl

		echo -e "Building OpenSSL Crypto and SSL libraries..."

		./Configure linux64-riscv64 \
			--prefix=$SYSROOT/usr --libdir=lib \
			--cross-compile-prefix=${CROSS_PREFIX}-
		make clean
		make -j
		make -j install_sw
	else
		echo -e "OpenSSL Crypto and SSL libraries already built"
	fi
}

# libaio
function cross_compile_libaio() {
	cd $CROSS_COMPILE_DIR

	if [ ! -d "$CROSS_COMPILE_DIR/libaio" ]; then
		echo -e "Downloading libaio library..."
		wget https://ftp.debian.org/debian/pool/main/liba/libaio/libaio_0.3.112.orig.tar.xz --no-check-certificate
		tar xvf libaio_0.3.112.orig.tar.xz
		mv libaio-0.3.112 libaio
	else
		echo -e "libaio library already downloaded"
	fi

	if [ ! -f "$SYSROOT/usr/lib/libaio.so" ]; then
		cd libaio

		echo -e "Building libaio library..."

		make clean
		CC=${CROSS_PREFIX}-gcc AR=${CROSS_PREFIX}-ar make -j
		make prefix=$SYSROOT/usr libdir=$SYSROOT/usr/lib install
	else
		echo -e "libaio library already built"
	fi
}

# ncurses
function cross_compile_ncurses() {
	cd $CROSS_COMPILE_DIR

	if [ ! -d "$CROSS_COMPILE_DIR/ncurses" ]; then
		echo -e "Downloading ncurses library..."
		wget https://ftp.gnu.org/pub/gnu/ncurses/ncurses-6.5.tar.gz --no-check-certificate
		tar xvf ncurses-6.5.tar.gz
		mv ncurses-6.5 ncurses
	else
		echo -e "ncurses library already downloaded"
	fi

	if [ ! -f "$SYSROOT/usr/lib/libncurses.a" ]; then
		cd ncurses

		echo -e "Building ncurses library..."

		# --disable-widec:   SPDK only uses narrow-char ncurses API
		# --without-cxx*:    ncurses 6.5 C++ bindings fail with GCC 15; SPDK uses C only
		./configure --host=${CROSS_PREFIX} \
			--prefix=$SYSROOT/usr --libdir=$SYSROOT/usr/lib \
			--disable-stripping --disable-widec \
			--enable-pc-files \
			--with-pkg-config-libdir=$SYSROOT/usr/lib/pkgconfig \
			--without-manpages --without-progs --without-tests \
			--without-cxx --without-cxx-binding
		make -j
		make -j install
	else
		echo -e "ncurses library already built"
	fi
}

# CUnit
function cross_compile_cunit() {
	cd $CROSS_COMPILE_DIR

	if [ ! -d "$CROSS_COMPILE_DIR/CUnit" ]; then
		echo -e "Downloading CUnit library..."
		git clone --depth=1 https://github.com/jacklicn/CUnit.git
	else
		echo -e "CUnit library already downloaded"
	fi

	if [ ! -f "$SYSROOT/usr/lib/libcunit.so" ]; then
		cd CUnit

		echo -e "Building CUnit library..."

		libtoolize --force
		aclocal
		autoheader
		automake --force-missing --add-missing
		autoconf
		./configure --host=${CROSS_PREFIX} \
			--prefix=$SYSROOT/usr --libdir=$SYSROOT/usr/lib
		make clean
		make -j
		make -j install
	else
		echo -e "CUnit library already built"
	fi
}

# DPDK
function cross_compile_dpdk() {
	cd $DPDK_DIR

	# install to riscv64/ not build/: dpdk/build triggers SPDK's in-tree dpdkbuild (no cross support)
	if [ ! -d "$DPDK_DIR/riscv64/lib" ]; then
		echo -e "Building DPDK libraries..."

		local cross_file=$CROSS_COMPILE_DIR/dpdk_riscv_cross.ini
		cat > "$cross_file" << EOF
[binaries]
c = '${CROSS_PREFIX}-gcc'
cpp = '${CROSS_PREFIX}-g++'
ar = '${CROSS_PREFIX}-ar'
strip = '${CROSS_PREFIX}-strip'
pkgconfig = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'riscv64'
cpu = '${TARGET_ARCH}'
endian = 'little'

[properties]
vendor_id = 'generic'
arch_id = 'generic'
pkg_config_libdir = '${SYSROOT}/usr/lib/pkgconfig'

[built-in options]
c_args = ['-I${SYSROOT}/usr/include']
c_link_args = ['-L${SYSROOT}/usr/lib', '-Wl,-rpath-link=${SYSROOT}/usr/lib']
EOF

		# disable compress/isal: ISA-L is built later by SPDK configure, so it isn't in sysroot when DPDK builds
		meson setup riscv64-build-gcc --cross-file "$cross_file" \
			-Dprefix=$DPDK_DIR/riscv64 \
			-Ddisable_drivers=compress/isal
		ninja -C riscv64-build-gcc
		ninja -C riscv64-build-gcc install
	else
		echo -e "DPDK libraries already built"
	fi
}

# SPDK
function cross_compile_spdk() {
	cd $SPDK_DIR

	if [ ! -f "$SPDK_DIR/isa-l/autogen.sh" ]; then
		git submodule update --init isa-l
	fi

	echo -e "Building SPDK libraries and binaries..."

	local xc_cflags="-I$SYSROOT/usr/include"
	local xc_ldflags="-L$SYSROOT/usr/lib -Wl,-rpath-link=$SYSROOT/usr/lib"

	# pin pkg-config to sysroot so configure probes and Makefile $(shell pkg-config ...) skip host .pc
	export PKG_CONFIG_PATH=""
	export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/pkgconfig"

	# --without-nvme-cuse: fuse is not cross-compiled into the sysroot
	CC=${CROSS_PREFIX}-gcc \
		CXX=${CROSS_PREFIX}-g++ \
		LD=${CROSS_PREFIX}-ld \
		CFLAGS="$xc_cflags" \
		CXXFLAGS="$xc_cflags" \
		LDFLAGS="$xc_ldflags" \
		./configure \
		--cross-prefix=${CROSS_PREFIX} \
		--without-vhost \
		--without-nvme-cuse \
		--with-dpdk=$DPDK_DIR/riscv64 \
		--target-arch=${TARGET_ARCH}

	if ! grep -qE '^CONFIG_ISAL\??=y$' mk/config.mk; then
		echo "error: CONFIG_ISAL is not y in mk/config.mk; riscv64 ISA-L support regressed."
		grep '^CONFIG_ISAL' mk/config.mk
		exit 1
	fi

	# ?= -> += so sysroot/DPDK flags still append when unit-test Makefiles pre-define CFLAGS
	# before including spdk.common.mk. Root cause: configure writes `CFLAGS?=...`.
	sed -i 's/^\(CFLAGS\|CXXFLAGS\|LDFLAGS\)?=/\1+=/' mk/cc.flags.mk
	if ! grep -q '^CFLAGS+=' mk/cc.flags.mk; then
		echo "error: failed to patch mk/cc.flags.mk (upstream format changed?)"
		exit 1
	fi

	make -j
}

mkdir -p $CROSS_COMPILE_DIR
mkdir -p $SYSROOT/usr/lib
mkdir -p $SYSROOT/usr/include

check_toolchain

cross_compile_packages=(numa uuid crypto_ssl libaio ncurses cunit dpdk spdk)

for index in "${cross_compile_packages[@]}"; do
	cross_compile_$index
done
