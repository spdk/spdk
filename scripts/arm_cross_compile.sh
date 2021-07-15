#!/usr/bin/env bash

# exit on errors
set -e

ROOT_DIR=$(readlink -f $(dirname $0))/../..
export CROSS_COMPILE_DIR=$ROOT_DIR/cross_compiling
export SPDK_DIR=$ROOT_DIR/spdk
export DPDK_DIR=$SPDK_DIR/dpdk

# Get Toolchain
function get_cc_toolchain() {
	cd $CROSS_COMPILE_DIR

	if [ ! -d "$CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu" ]; then
		echo -e "Getting ARM Cross Compiler Toolchain..."
		wget https://developer.arm.com/-/media/Files/downloads/gnu-a/10.2-2020.11/binrel/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu.tar.xz --no-check-certificate
		tar xvf gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu.tar.xz
	else
		echo -e "ARM Cross Compiler Toolchain already downloaded"
	fi

	export PATH=$PATH:$CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/bin
}

# NUMA
function cross_compile_numa() {
	cd $CROSS_COMPILE_DIR

	# Download NUMA library
	if [ ! -d "$CROSS_COMPILE_DIR/numactl" ]; then
		echo -e "Downloading NUMA library..."
		git clone https://github.com/numactl/numactl.git
		cd numactl/
		git checkout v2.0.13 -b v2.0.13
	else
		echo -e "NUMA library already downloaded"
		cd numactl/
	fi

	# Build NUMA library
	if [ ! -d "$CROSS_COMPILE_DIR/numactl/build" ]; then
		echo -e "Building NUMA library..."
		./autogen.sh
		autoconf -i
		mkdir build
		./configure --host=aarch64-none-linux-gnu CC=aarch64-none-linux-gnu-gcc --prefix=$CROSS_COMPILE_DIR/numactl/build
		make -j install

		# Copy NUMA related dependencies
		echo -e "Copying NUMA library dependencies..."

		cp build/include/numa*.h $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc/usr/include/
		cp build/lib/libnuma.a $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/
		cp build/lib/libnuma.so $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/
	else
		echo -e "NUMA library already built"
	fi
}

# util-linux UUID
function cross_compile_uuid() {
	cd $CROSS_COMPILE_DIR

	# Download util-linux UUID library
	if [ ! -d "$CROSS_COMPILE_DIR/util-linux" ]; then
		echo -e "Downloading util-linux UUID library..."
		git clone https://github.com/karelzak/util-linux.git
	else
		echo -e "util-linux UUID library already downloaded"
	fi

	if [ ! -d "$CROSS_COMPILE_DIR/util-linux/.libs" ]; then
		cd util-linux/

		# Build util-linux UUID library
		echo -e "Building util-linux UUID library..."

		./autogen.sh
		CC=aarch64-none-linux-gnu-gcc CXX=aarch64-none-linux-gnu-g++ LD=aarch64-none-linux-gnu-ld CFLAGS+=-Wl,-rpath=$CROSS_COMPILE_DIR/util-linux/.libs ./configure --host=aarch64-none-linux-gnu --without-tinfo --without-ncurses --without-ncursesw --disable-mount --disable-libmount --disable-pylibmount --disable-libblkid --disable-fdisks --disable-libfdisk
		make clean
		make -j

		# Copy util-linux UUID related dependencies
		echo -e "Copying util-linux UUID library dependencies..."

		cp .libs/libuuid.so.1.3.0 $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/libuuid.so
		mkdir -p $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc/usr/include/uuid/
		cp libuuid/src/uuid.h $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc/usr/include/uuid/
	else
		echo -e "util-linux UUID library already built"
	fi
}

# Openssl Crypto and SSL
function cross_compile_crypto_ssl() {
	cd $CROSS_COMPILE_DIR

	# Download Openssl Crypto and SSL libraries
	if [ ! -d "$CROSS_COMPILE_DIR/openssl" ]; then
		echo -e "Downloading Openssl Crypto and SSL libraries..."
		git clone https://github.com/openssl/openssl.git
	else
		echo -e "Openssl Crypto and SSL libraries already downloaded"
	fi

	if [ ! -d "$CROSS_COMPILE_DIR/openssl/build" ]; then
		cd openssl

		# Build Openssl Crypto and SSL libraries
		echo -e "Building Openssl Crypto and SSL libraries..."

		mkdir build
		./Configure linux-aarch64 --prefix=$CROSS_COMPILE_DIR/openssl/build --cross-compile-prefix=aarch64-none-linux-gnu-
		make -j
		make -j install

		# Copy Openssl Crypto and SSL related dependencies
		echo -e "Copying Openssl Crypto and SSL libraries dependencies..."

		cp -fr build/include/openssl $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc/usr/include/
		cp build/lib/libcrypto.so.3 $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/libcrypto.so
		cp build/lib/libcrypto.so.3 $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/libcrypto.so.3
		cp build/lib/libssl.so.3 $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/libssl.so
	else
		echo -e "Openssl Crypto and SSL libraries already built"
	fi
}

# Libaio
function cross_compile_libaio() {
	cd $CROSS_COMPILE_DIR

	# Download libaio library
	if [ ! -d "$CROSS_COMPILE_DIR/libaio" ]; then
		echo -e "Downloading libaio library..."

		wget https://ftp.debian.org/debian/pool/main/liba/libaio/libaio_0.3.112.orig.tar.xz --no-check-certificate
		tar xvf libaio_0.3.112.orig.tar.xz
		mv libaio-0.3.112 libaio
	else
		echo -e "libaio library already downloaded"
	fi

	if [ ! -d "$CROSS_COMPILE_DIR/libaio/build" ]; then
		cd libaio

		# Build libaio library
		echo -e "Building libaio library..."

		mkdir build
		CC=aarch64-none-linux-gnu-gcc CXX=aarch64-none-linux-gnu-g++ LD=aarch64-none-linux-gnu-ld make -j
		make -j install DESTDIR=$CROSS_COMPILE_DIR/libaio/build

		# Copy libaio related dependencies
		echo -e "Copying libaio library dependencies..."

		cp build/usr/include/libaio.h $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc/usr/include/
		cp build/usr/lib/libaio.so.1.0.1 $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/libaio.so
	else
		echo -e "libaio library already built"
	fi
}

# Ncurses
function cross_compile_ncurses() {
	cd $CROSS_COMPILE_DIR

	# Download ncurses library
	if [ ! -d "$CROSS_COMPILE_DIR/ncurses" ]; then
		echo -e "Downloading ncurses library..."

		wget https://ftp.gnu.org/pub/gnu/ncurses/ncurses-6.2.tar.gz --no-check-certificate
		tar xvf ncurses-6.2.tar.gz
		mv ncurses-6.2 ncurses
	else
		echo -e "ncurses library already downloaded"
	fi

	if [ ! -d "$CROSS_COMPILE_DIR/ncurses_build" ]; then
		mkdir ncurses_build

		# Build ncurses library
		echo -e "Building ncurses library..."

		(cd ncurses && ./configure --host=aarch64-none-linux-gnu --prefix=$CROSS_COMPILE_DIR/ncurses_build --disable-stripping && make -j install)

		# Copy ncurses related dependencies
		echo -e "Copying ncurses library dependencies..."

		cp ncurses_build/include/ncurses/ncurses.h $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc/usr/include/
		cp ncurses_build/include/ncurses/curses.h $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc/usr/include/
		cp -fr ncurses_build/include/ncurses $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc/usr/include/
		cp ncurses_build/include/ncurses/menu.h $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc/usr/include/
		cp ncurses_build/include/ncurses/eti.h $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc/usr/include/
		cp ncurses_build/include/ncurses/panel.h $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc/usr/include/
		cp ncurses_build/lib/libncurses* $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/
		cp ncurses_build/lib/libmenu.a $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/
		cp ncurses_build/lib/libpanel.a $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/
	else
		echo -e "ncurses library already built"
	fi

}

# CUnit
function cross_compile_cunit() {
	cd $CROSS_COMPILE_DIR

	# Download cunit library
	if [ ! -d "$CROSS_COMPILE_DIR/CUnit" ]; then
		echo -e "Downloading cunit library..."

		git clone https://github.com/jacklicn/CUnit.git
	else
		echo -e "cunit library already downloaded"
	fi

	if [ ! -d "$CROSS_COMPILE_DIR/CUnit/build" ]; then
		cd CUnit

		# Build cunit library
		echo -e "Building cunit library..."

		mkdir build
		libtoolize --force
		aclocal
		autoheader
		automake --force-missing --add-missing
		autoconf
		./configure --host=aarch64-none-linux-gnu --prefix=$CROSS_COMPILE_DIR/CUnit/build
		make -j
		make -j install

		# Copy cunit related dependencies
		echo -e "Copying cunit library dependencies..."

		cp -fr build/include/CUnit $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc/usr/include/
		cp build/lib/libcunit.a $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/
		cp build/lib/libcunit.so.1.0.1 $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/libcunit.so
	else
		echo -e "cunit library already built"
	fi
}

# ISA-L
function cross_compile_isal() {
	cd $SPDK_DIR

	if [ ! -d "$SPDK_DIR/isa-l/build" ]; then
		# Build ISA-L library
		echo -e "Building ISA-L library..."

		cd isa-l
		./autogen.sh
		mkdir -p build/lib
		ac_cv_func_malloc_0_nonnull=yes ac_cv_func_realloc_0_nonnull=yes ./configure --prefix=$SPDK_DIR/isa-l/build --libdir=$SPDK_DIR/isa-l/build/lib --host=aarch64-none-linux-gnu
		make -j
		make -j install

		# Copy ISAL related dependencies
		echo -e "Copying ISA-L library dependencies..."

		cp -fr build/include/isa-l $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc/usr/include/
		cp build/include/isa-l.h $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc/usr/include/
		cp build/lib/libisal.a $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/
		cp build/lib/libisal.so.2.0.30 $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/
		ln -sf $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/libisal.so.2.0.30 $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/libisal.so
		ln -sf $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/libisal.so.2.0.30 $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/libisal.so.2
	else
		echo -e "ISA-L library already built"
	fi
}

# DPDK
function cross_compile_dpdk() {
	cd $DPDK_DIR

	if [ ! -d "$DPDK_DIR/build" ]; then
		# Build DPDK libraries
		echo -e "Building DPDK libraries..."

		apt install pkg-config-aarch64-linux-gnu
		meson aarch64-build-gcc --cross-file config/arm/arm64_armv8_linux_gcc -Dprefix=$DPDK_DIR/build
		ninja -C aarch64-build-gcc
		ninja -C aarch64-build-gcc install
		cd ..

		# Copy DPDK related dependencies
		echo -e "Copying DPDK libraries dependencies..."

		cp -fr dpdk/build/bin dpdk/aarch64-build-gcc/
		cp -fr dpdk/build/include dpdk/aarch64-build-gcc/
		cp -fr dpdk/build/share dpdk/aarch64-build-gcc/
		cp -fr dpdk/build/lib/* dpdk/aarch64-build-gcc/lib/
		cp $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/libcrypto.so.3 dpdk/aarch64-build-gcc/lib/
		cp $CROSS_COMPILE_DIR/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/lib/gcc/aarch64-none-linux-gnu/10.2.1/libcrypto.so dpdk/aarch64-build-gcc/lib/
	else
		echo -e "DPDK libraries already built"
	fi
}

# SPDK
function cross_compile_spdk() {
	cd $SPDK_DIR

	# Build SPDK libraries and binaries
	echo -e "Building SPDK libraries and binaries..."

	CC=aarch64-none-linux-gnu-gcc CXX=aarch64-none-linux-gnu-g++ LD=aarch64-none-linux-gnu-ld CFLAGS+=-I$DPDK_DIR/aarch64-build-gcc/include ./configure --cross-prefix=aarch64-none-linux-gnu --without-vhost --with-dpdk=$DPDK_DIR/aarch64-build-gcc --target-arch=armv8-a

	make -j
}

mkdir -p $CROSS_COMPILE_DIR

get_cc_toolchain

cross_compile_packages=(numa uuid crypto_ssl libaio ncurses cunit isal dpdk spdk)

for index in ${cross_compile_packages[*]}; do
	cross_compile_$index
done
